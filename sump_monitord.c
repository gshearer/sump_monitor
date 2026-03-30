// sump_monitord.c -- sump pit water level monitoring daemon
//
// Monitors a water level sensor via MCP3008 ADC over SPI and exposes
// state via a Unix domain socket. Detects pump cycles (water rises
// then falls) and triggers alarms when water stays critically high
// (pump failure).
//
// Build:
//   gcc -Wall -s -o sump_monitord sump_monitord.c
//
// Socket output (4 lines):
//   1. Current ADC reading (0-1023)
//   2. Alarm state (0=normal, 1=alarm)
//   3. Pump cycle count (successful rise-then-fall events)
//   4. Alert execution count

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <errno.h>
#include <limits.h>
#include <linux/spi/spidev.h>

#define SOCKET_PATH  "/tmp/sump_monitor.sock"
#define SPI_DEVICE   "/dev/spidev0.0"
#define SPI_SPEED_HZ 1000000
#define SPI_MODE_VAL SPI_MODE_0
#define SPI_BPW      8
#define ADC_CHANNEL  0

// sensor and alarm state
static int32_t  current_adc = 0;
static bool     water_present = false;
static bool     alarm_active = false;
static bool     alarm_notified = false;
static time_t   high_water_since = 0;
static time_t   last_alert_time = 0;
static int32_t  pump_cycle_count = 0;
static int32_t  alert_exec_count = 0;
static const char *notify_script_path = NULL;

// signal handler flags
static volatile sig_atomic_t keep_running = 1;
static volatile sig_atomic_t debug_enable = 0;
static bool debug_prev = false;


// handle_signal -- process termination and debug toggle signals
// arg signum: signal number received (SIGINT, SIGTERM, or SIGUSR1)
static void
handle_signal(int32_t signum)
{
  if(signum == SIGINT || signum == SIGTERM)
    keep_running = 0;

  else if(signum == SIGUSR1)
    debug_enable = !debug_enable;
}


// spi_open -- initialize the SPI bus for MCP3008 communication
// returns: file descriptor on success, -1 on failure
static int32_t
spi_open(void)
{
  int32_t fd = open(SPI_DEVICE, O_RDWR);
  if(fd < 0)
  {
    perror("failed to open " SPI_DEVICE);
    return -1;
  }

  unsigned char mode = SPI_MODE_VAL;
  unsigned char bpw  = SPI_BPW;
  unsigned int speed  = SPI_SPEED_HZ;

  if(ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0)
  {
    perror("failed to set SPI mode");
    close(fd);
    return -1;
  }

  if(ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bpw) < 0)
  {
    perror("failed to set SPI bits per word");
    close(fd);
    return -1;
  }

  if(ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0)
  {
    perror("failed to set SPI speed");
    close(fd);
    return -1;
  }

  return fd;
}


// mcp3008_read -- perform a single-ended ADC read on the given channel
// returns: 10-bit ADC value (0-1023) on success, -1 on failure
// arg fd: SPI file descriptor
// arg channel: MCP3008 channel number (0-7)
static int32_t
mcp3008_read(int32_t fd, uint32_t channel)
{
  unsigned char tx[3];
  unsigned char rx[3];

  tx[0] = 0x01;
  tx[1] = 0x80 | ((channel & 0x07) << 4);
  tx[2] = 0x00;

  struct spi_ioc_transfer xfer;
  memset(&xfer, 0, sizeof(xfer));
  xfer.tx_buf        = (unsigned long)tx;
  xfer.rx_buf        = (unsigned long)rx;
  xfer.len           = 3;
  xfer.speed_hz      = SPI_SPEED_HZ;
  xfer.bits_per_word = SPI_BPW;

  if(ioctl(fd, SPI_IOC_MESSAGE(1), &xfer) < 0)
  {
    perror("SPI transfer failed");
    return -1;
  }

  return ((rx[1] & 0x03) << 8) | rx[2];
}


// trigger_notification -- fork/exec the notification script
// arg state_str: "ALARM" or "NORMAL" passed as first argument to the script
static void
trigger_notification(const char *state_str)
{
  pid_t pid;

  fprintf(stderr, "NOTIFICATION: %s! Executing %s %s\n",
          state_str, notify_script_path, state_str);

  pid = fork();
  if(pid < 0)
  {
    perror("fork failed for notification script");
    return;
  }

  if(pid == 0)
  {
    execl(notify_script_path, notify_script_path, state_str, (char *)NULL);
    perror("execl failed for notification script");
    _exit(EXIT_FAILURE);
  }
}


// env_int -- read an integer from an environment variable
// returns: the parsed integer, or fallback if unset or invalid
// arg name: environment variable name
// arg fallback: default value when the variable is absent or invalid
static int32_t
env_int(const char *name, int32_t fallback)
{
  const char *val = getenv(name);
  char *end = NULL;
  long parsed;

  if(!val)
    return fallback;

  errno = 0;
  parsed = strtol(val, &end, 10);

  if(errno != 0 || end == val || *end != '\0')
  {
    fprintf(stderr, "warning: invalid value for %s='%s', using default %d\n",
            name, val, fallback);
    return fallback;
  }

  if(parsed < INT32_MIN || parsed > INT32_MAX)
  {
    fprintf(stderr, "warning: value for %s out of range, using default %d\n",
            name, fallback);
    return fallback;
  }

  return (int32_t)parsed;
}


// main -- daemon entry point
// Sets up signal handlers, SPI, and a Unix domain socket,
// then enters the polling loop to monitor sensor state.
// returns: EXIT_SUCCESS on clean shutdown, EXIT_FAILURE on error
int
main(void)
{
  int32_t server_sock = -1;
  int32_t client_sock;
  struct sockaddr_un server_addr;
  struct pollfd fds[1];
  time_t last_check_time = 0;
  struct sigaction sa;

  // 1. setup signal handlers
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = handle_signal;

  if(sigemptyset(&sa.sa_mask) < 0)
  {
    perror("sigemptyset failed");
    return EXIT_FAILURE;
  }

  if(sigaction(SIGINT, &sa, NULL) < 0 ||
     sigaction(SIGTERM, &sa, NULL) < 0 ||
     sigaction(SIGUSR1, &sa, NULL) < 0)
  {
    perror("sigaction failed");
    return EXIT_FAILURE;
  }

  // ignore SIGPIPE (socket writes) and SIGCHLD (notification children)
  sa.sa_handler = SIG_IGN;

  if(sigaction(SIGPIPE, &sa, NULL) < 0 ||
     sigaction(SIGCHLD, &sa, NULL) < 0)
  {
    perror("sigaction failed for SIGPIPE/SIGCHLD");
    return EXIT_FAILURE;
  }

  // configuration from environment
  notify_script_path = getenv("SUMP_NOTIFY_SCRIPT");
  if(notify_script_path == NULL)
    notify_script_path = "/usr/local/bin/sump_notify.sh";

  const int32_t poll_interval    = env_int("SUMP_POLL_INTERVAL", 1);
  const int32_t water_threshold  = env_int("SUMP_WATER_THRESHOLD", 100);
  const int32_t alarm_level      = env_int("SUMP_ALARM_LEVEL", 600);
  const int32_t alarm_delay      = env_int("SUMP_ALARM_DELAY", 300);
  const int32_t alert_holddown   = env_int("SUMP_ALERT_HOLDDOWN", 300);
  const int32_t state_threshold  = env_int("SUMP_STATE_THRESHOLD", 3);

  // 2. SPI setup for MCP3008
  int32_t spi_fd = spi_open();
  if(spi_fd < 0)
    return EXIT_FAILURE;

  // 3. setup Unix domain socket
  server_sock = socket(AF_UNIX, SOCK_STREAM, 0);
  if(server_sock < 0)
  {
    perror("socket creation failed");
    close(spi_fd);
    return EXIT_FAILURE;
  }

  unlink(SOCKET_PATH);
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sun_family = AF_UNIX;
  strncpy(server_addr.sun_path, SOCKET_PATH, sizeof(server_addr.sun_path) - 1);
  server_addr.sun_path[sizeof(server_addr.sun_path) - 1] = '\0';

  if(bind(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
  {
    perror("bind failed");
    close(server_sock);
    close(spi_fd);
    return EXIT_FAILURE;
  }

  if(listen(server_sock, 5) < 0)
  {
    perror("listen failed");
    close(server_sock);
    unlink(SOCKET_PATH);
    close(spi_fd);
    return EXIT_FAILURE;
  }

  if(chmod(SOCKET_PATH, 0666) < 0)
    perror("chmod on socket path failed");

  fds[0].fd = server_sock;
  fds[0].events = POLLIN;

  fprintf(stderr, "Daemon active. poll=%ds water_thresh=%d alarm_level=%d "
          "alarm_delay=%ds alert_holddown=%ds state_thresh=%d Socket: %s\n",
          poll_interval, water_threshold, alarm_level,
          alarm_delay, alert_holddown, state_threshold, SOCKET_PATH);

  // debounce counters
  int32_t consecutive_wet = 0;
  int32_t consecutive_dry = 0;

  // 4. main event loop
  while(keep_running)
  {
    int32_t ret = poll(fds, 1, poll_interval * 1000);

    // handle signal interrupt during poll
    if(ret < 0)
    {
      if(errno == EINTR)
      {
        if(debug_enable != debug_prev)
        {
          fprintf(stderr, "debug output %s\n",
                  debug_enable ? "enabled" : "disabled");
          debug_prev = debug_enable;
        }

        continue;
      }

      perror("poll error");
      break;
    }

    // handle incoming socket connection
    if(ret > 0 && (fds[0].revents & POLLIN))
    {
      client_sock = accept(server_sock, NULL, NULL);
      if(client_sock >= 0)
      {
        char response[96];
        ssize_t nwritten;

        snprintf(response, sizeof(response), "%d\n%d\n%d\n%d\n",
                 current_adc, alarm_active ? 1 : 0,
                 pump_cycle_count, alert_exec_count);

        nwritten = write(client_sock, response, strlen(response));
        if(nwritten < 0)
          perror("write to client socket failed");

        close(client_sock);
      }
    }

    // periodic sensor read
    time_t now = time(NULL);

    if(now - last_check_time >= poll_interval)
    {
      last_check_time = now;

      int32_t val = mcp3008_read(spi_fd, ADC_CHANNEL);
      if(val < 0)
      {
        perror("ADC read failed");
        continue;
      }

      current_adc = val;

      if(debug_enable)
        fprintf(stderr, "[DEBUG] adc=%d water=%s alarm=%s "
                "consec_wet=%d consec_dry=%d cycles=%d alerts=%d\n",
                val, water_present ? "yes" : "no",
                alarm_active ? "ALARM" : "normal",
                consecutive_wet, consecutive_dry,
                pump_cycle_count, alert_exec_count);

      // --- water detection with debounce ---

      if(val >= water_threshold)
      {
        consecutive_wet++;
        consecutive_dry = 0;

        if(!water_present && consecutive_wet >= state_threshold)
        {
          water_present = true;
          fprintf(stderr, "Water detected (ADC=%d, threshold=%d)\n",
                  val, water_threshold);
        }
      }
      else
      {
        consecutive_dry++;
        consecutive_wet = 0;

        if(water_present && consecutive_dry >= state_threshold)
        {
          water_present = false;
          fprintf(stderr, "Water receded (ADC=%d). Pump cycle completed (#%d)\n",
                  val, pump_cycle_count + 1);
          pump_cycle_count++;

          // clear alarm if water has receded
          if(alarm_active)
          {
            alarm_active = false;
            high_water_since = 0;
            fprintf(stderr, "Alarm cleared, water level normal\n");

            if(alarm_notified)
            {
              alarm_notified = false;
              trigger_notification("NORMAL");
            }
          }

          high_water_since = 0;
        }
      }

      // --- alarm detection: sustained high water ---

      if(water_present && val >= alarm_level)
      {
        if(high_water_since == 0)
        {
          high_water_since = now;
          fprintf(stderr, "High water detected (ADC=%d >= %d), "
                  "watching for %ds\n", val, alarm_level, alarm_delay);
        }

        if(!alarm_active && (now - high_water_since) >= alarm_delay)
        {
          alarm_active = true;
          fprintf(stderr, "ALARM: Water at critical level for %ds! "
                  "(ADC=%d)\n", alarm_delay, val);
        }

        if(alarm_active && (now - last_alert_time) >= alert_holddown)
        {
          trigger_notification("ALARM");
          alarm_notified = true;
          last_alert_time = now;
          alert_exec_count++;
        }
      }
      else
      {
        // water present but below alarm level, or no water --
        // reset the high-water timer
        high_water_since = 0;
      }
    }
  }

  // 5. graceful cleanup
  fprintf(stderr, "\nShutting down gracefully. Releasing resources...\n");
  close(spi_fd);
  close(server_sock);
  unlink(SOCKET_PATH);

  return EXIT_SUCCESS;
}
