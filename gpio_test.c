/*
 * gpio_test.c - MCP3008 ADC water sensor test utility
 *
 * Reads a resistive water level sensor connected to MCP3008 CH0
 * via the Linux spidev interface (/dev/spidev0.0).
 *
 * Usage:
 *   ./gpio_test              Single reading
 *   ./gpio_test -l           Loop with 1-second interval (Ctrl+C to stop)
 *   ./gpio_test -d           Single reading with raw SPI debug bytes
 *   ./gpio_test --loopback   SPI loopback test (short MOSI to MISO, no MCP3008)
 *
 * Build:
 *   gcc -Wall -s -o gpio_test gpio_test.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>

#define SPI_DEVICE   "/dev/spidev0.0"
#define SPI_SPEED_HZ 1000000
#define SPI_MODE     SPI_MODE_0
#define SPI_BPW      8
#define ADC_CHANNEL  0
#define VREF         3.3

static volatile sig_atomic_t running = 1;
static int debug = 0;

static void sig_handler(int sig)
{
	(void)sig;
	running = 0;
}

static int spi_open(void)
{
	int fd = open(SPI_DEVICE, O_RDWR);
	if (fd < 0) {
		perror("Failed to open " SPI_DEVICE);
		return -1;
	}

	unsigned char mode = SPI_MODE;
	unsigned char bpw  = SPI_BPW;
	unsigned int speed  = SPI_SPEED_HZ;

	if (ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0) {
		perror("Failed to set SPI mode");
		close(fd);
		return -1;
	}
	if (ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bpw) < 0) {
		perror("Failed to set bits per word");
		close(fd);
		return -1;
	}
	if (ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
		perror("Failed to set SPI speed");
		close(fd);
		return -1;
	}

	return fd;
}

static int mcp3008_read(int fd, unsigned int channel)
{
	/*
	 * MCP3008 SPI protocol (single-ended):
	 *   TX: [0x01] [0x80 | (channel << 4)] [0x00]
	 *   RX: [xxxx] [0000 00XX]             [XXXX XXXX]
	 *                     ^^ upper 2 bits    ^^^^^^^^ lower 8 bits
	 */
	unsigned char tx[3];
	unsigned char rx[3];

	tx[0] = 0x01;                          /* start bit */
	tx[1] = 0x80 | ((channel & 0x07) << 4); /* single-ended + channel */
	tx[2] = 0x00;

	struct spi_ioc_transfer xfer;
	memset(&xfer, 0, sizeof(xfer));
	xfer.tx_buf        = (unsigned long)tx;
	xfer.rx_buf        = (unsigned long)rx;
	xfer.len           = 3;
	xfer.speed_hz      = SPI_SPEED_HZ;
	xfer.bits_per_word = SPI_BPW;

	if (ioctl(fd, SPI_IOC_MESSAGE(1), &xfer) < 0) {
		perror("SPI transfer failed");
		return -1;
	}

	if (debug)
		printf("  SPI TX: [%02X %02X %02X]  RX: [%02X %02X %02X]\n",
		       tx[0], tx[1], tx[2], rx[0], rx[1], rx[2]);

	return ((rx[1] & 0x03) << 8) | rx[2];
}

static void print_reading(int adc_value)
{
	double voltage = (adc_value / 1023.0) * VREF;

	printf("ADC: %4d / 1023  |  Voltage: %.2fV  |  ", adc_value, voltage);

	if (adc_value < 100)
		printf("DRY (no water detected)\n");
	else if (adc_value < 400)
		printf("DAMP (minimal contact)\n");
	else
		printf("WET (water detected)\n");
}

static int spi_loopback(int fd)
{
	/*
	 * SPI loopback test: short MOSI (Pin 19) to MISO (Pin 21).
	 * Sends known bytes and checks if they come back.
	 * No MCP3008 needed — tests the SPI bus itself.
	 */
	unsigned char tx[3] = {0xAA, 0x55, 0xF0};
	unsigned char rx[3] = {0};

	struct spi_ioc_transfer xfer;
	memset(&xfer, 0, sizeof(xfer));
	xfer.tx_buf        = (unsigned long)tx;
	xfer.rx_buf        = (unsigned long)rx;
	xfer.len           = 3;
	xfer.speed_hz      = SPI_SPEED_HZ;
	xfer.bits_per_word = SPI_BPW;

	printf("SPI Loopback Test (MOSI → MISO)\n");
	printf("────────────────────────────────\n");
	printf("Short Pi Pin 19 (MOSI) to Pin 21 (MISO) with a jumper wire.\n");
	printf("Disconnect the MCP3008 first.\n\n");

	if (ioctl(fd, SPI_IOC_MESSAGE(1), &xfer) < 0) {
		perror("SPI transfer failed");
		return EXIT_FAILURE;
	}

	printf("  TX: [%02X %02X %02X]\n", tx[0], tx[1], tx[2]);
	printf("  RX: [%02X %02X %02X]\n", rx[0], rx[1], rx[2]);

	if (tx[0] == rx[0] && tx[1] == rx[1] && tx[2] == rx[2]) {
		printf("\n  PASS: SPI bus is working correctly.\n");
		return EXIT_SUCCESS;
	} else {
		printf("\n  FAIL: Data mismatch. Check:\n");
		printf("    - SPI enabled?  ls /dev/spidev0.*\n");
		printf("    - MOSI (Pin 19) shorted to MISO (Pin 21)?\n");
		printf("    - No other device on the bus?\n");
		return EXIT_FAILURE;
	}
}

int main(int argc, char *argv[])
{
	int loop_mode = 0;
	int loopback  = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-l") == 0)
			loop_mode = 1;
		else if (strcmp(argv[i], "-d") == 0)
			debug = 1;
		else if (strcmp(argv[i], "--loopback") == 0)
			loopback = 1;
	}

	int fd = spi_open();
	if (fd < 0)
		return EXIT_FAILURE;

	if (loopback) {
		int rc = spi_loopback(fd);
		close(fd);
		return rc;
	}

	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);

	printf("MCP3008 Water Sensor Test (CH%d, VREF=%.1fV)\n", ADC_CHANNEL, VREF);
	printf("─────────────────────────────────────────────\n");

	if (loop_mode)
		printf("Looping every 1s (Ctrl+C to stop)\n\n");

	do {
		int val = mcp3008_read(fd, ADC_CHANNEL);
		if (val < 0) {
			close(fd);
			return EXIT_FAILURE;
		}
		print_reading(val);

		if (loop_mode)
			sleep(1);
	} while (loop_mode && running);

	close(fd);
	return EXIT_SUCCESS;
}
