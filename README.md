Sump Monitor

A high-performance, bare-metal POSIX C daemon designed for Raspberry Pi to monitor sump pit water levels using an MCP3008 ADC and a resistive water level sensor over SPI.

The daemon reads analog water levels, detects pump cycles (water rises then recedes), and triggers alarms when water remains critically high for a sustained period (indicating pump failure). When an alarm condition is confirmed, the system executes a customizable notification script with state arguments (ALARM or NORMAL) and tracks pump cycles and alert executions.

It also features a Unix domain socket for seamless integration with NMS systems (like LibreNMS or Zabbix) via Net-SNMP.

Hardware Setup

MCP3008 Wiring

The MCP3008 is a 10-bit 8-channel ADC that communicates over SPI. All connections run at 3.3V.

```
          MCP3008
       +----U----+
 CH0  1|         |16  VDD ---- Pi 3.3V (Pin 1)
 CH1  2|         |15  VREF --- Pi 3.3V (Pin 1)
 CH2  3|         |14  AGND --- Pi GND  (Pin 6)
 CH3  4|         |13  CLK ---- Pi SCLK (Pin 23 / GPIO 11)
 CH4  5|         |12  DOUT --- Pi MISO (Pin 21 / GPIO 9)
 CH5  6|         |11  DIN ---- Pi MOSI (Pin 19 / GPIO 10)
 CH6  7|         |10  CS ----- Pi CE0  (Pin 24 / GPIO 8)
 CH7  8|         | 9  DGND --- Pi GND  (Pin 6)
       +─────────+
```

Water Sensor Wiring

| Sensor Pin | Connection |
|---|---|
| S (signal) | MCP3008 CH0 (pin 1) |
| + (VCC) | Pi 3.3V (Pin 1) |
| - (GND) | Pi GND (Pin 6) |

Power & Audio

- Audio: USB-powered speaker (e.g., Adafruit Mini USB Speaker).
- Power: To ensure monitoring during severe storms, use a battery backup system.

Prerequisites

This project requires the latest Raspberry Pi OS (Bookworm or newer). SPI must be enabled.

```Bash
sudo apt update && sudo apt dist-upgrade -y
sudo apt install -y git build-essential alsa-utils curl snmpd snmp snmp-mibs-downloader
```

Enable SPI via `raspi-config`:
```Bash
sudo raspi-config
# Interface Options -> SPI -> Enable
```

Verify SPI is available:
```Bash
ls /dev/spidev0.*
# Should show /dev/spidev0.0
```

Installation

1. Clone and Compile

Clone the repository and compile the daemon. No external libraries are required.
```Bash
git clone https://github.com/gshearer/sump_monitor
cd sump_monitor
gcc -Wall -s -o sump_monitord sump_monitord.c
```

2. System Integration

Deploy the binaries, audio assets, and the systemd service file into the standard Linux directories.
```Bash
# Create sound directory and move sound file
sudo mkdir -p /usr/local/share/sound
sudo mv sump_alert.wav /usr/local/share/sound/
```

Install executables and service file:

1. Edit sump_notify.sh, update alsa commands with your sound device (usb speaker, headphone jack, etc)
2. Update with your ntfy.sh subject or some other alerting mechanism

```Bash
sudo mv sump_monitord sump_notify.sh /usr/local/bin/
sudo mv sump_monitor.service /etc/systemd/system/
```

3. Permissions & Security

Lock down the daemon and the notification script (which may contain API keys) so they are only accessible by root.
```Bash
sudo chown root:root /usr/local/bin/sump_notify.sh /usr/local/bin/sump_monitord /etc/systemd/system/sump_monitor.service /usr/local/share/sound/sump_alert.wav
sudo chmod 0700 /usr/local/bin/sump_notify.sh
sudo chmod 0500 /usr/local/bin/sump_monitord
sudo chmod 0644 /etc/systemd/system/sump_monitor.service /usr/local/share/sound/sump_alert.wav
```

Configuration

All configuration is done via environment variables set in `sump_monitor.service`. Edit the service file to customize:

| Environment Variable | Purpose | Default |
|---|---|---|
| `SUMP_NOTIFY_SCRIPT` | Path to notification script | `/usr/local/bin/sump_notify.sh` |
| `SUMP_POLL_INTERVAL` | Sensor poll interval in seconds | `1` |
| `SUMP_WET_THRESHOLD` | ADC value at or above which water is considered detected | `300` |
| `SUMP_DRY_THRESHOLD` | ADC value at or below which water is considered receded | `50` |
| `SUMP_ADC_SAMPLES` | Number of ADC samples per poll, median filtered | `5` |
| `SUMP_ALARM_DELAY` | Seconds water must remain in wet state before alarm triggers | `300` |
| `SUMP_ALERT_HOLDDOWN` | Minimum seconds between repeated alert executions | `300` |
| `SUMP_STATE_THRESHOLD` | Consecutive polls to confirm a state change (debounce) | `3` |

Note: I place the water sensor slightly deeper than when the pump kicks on. This allows the system to detect water and track how many times the pump cycles. If the water doesn't drain within SUMP_POLL_INTERVAL, the alarm is triggered.

After editing, reload and restart:
```Bash
sudo systemctl daemon-reload
sudo systemctl restart sump_monitor.service
```

SNMP Integration

The daemon's Unix socket outputs four lines of data:

| Line | Meaning |
|---|---|
| 1 | Current ADC reading (0-1023) |
| 2 | Alarm state (0 = normal, 1 = alarm) |
| 3 | Pump cycle count |
| 4 | Alert script execution count |

Add the extension to snmpd: (you may want to adjust community name, view permissions etc)
```Bash
# add the extend and set cache to 1 second
printf 'extend sumpMetrics /usr/bin/bash -c '"'"'/bin/echo "" | /usr/bin/nc -U /tmp/sump_monitor.sock'"'"'\nextendCacheTime sumpMetrics 1\n' | sudo tee /etc/snmp/snmpd.conf.d/sump_monitor.conf
# allow those oids to be viewed
sudo sed -i '/view   systemonly  included   .1.3.6.1.2.1.25.1/a view   systemonly  included   .1.3.6.1.4.1.8072.1.3' /etc/snmp/snmpd.conf
# allow the snmp service to be queried externally
sudo sed -i 's/^agentaddress.*/agentaddress udp:161/' /etc/snmp/snmpd.conf
sudo systemctl enable --now snmpd.service
sudo systemctl enable --now sump_monitor.service
```

Query the OIDs from your NMS:

```Bash
# Current ADC reading (0-1023)
snmpget -v2c -c public <IP> 'NET-SNMP-EXTEND-MIB::nsExtendOutLine."sumpMetrics".1'

# Alarm state (0=normal, 1=alarm)
snmpget -v2c -c public <IP> 'NET-SNMP-EXTEND-MIB::nsExtendOutLine."sumpMetrics".2'

# Pump cycle count
snmpget -v2c -c public <IP> 'NET-SNMP-EXTEND-MIB::nsExtendOutLine."sumpMetrics".3'

# Alert script execution count
snmpget -v2c -c public <IP> 'NET-SNMP-EXTEND-MIB::nsExtendOutLine."sumpMetrics".4'
```

Testing

GPIO Test Utility

A standalone test utility (`gpio_test.c`) is included for verifying the MCP3008 and water sensor:

```Bash
gcc -Wall -s -o gpio_test gpio_test.c

# Single reading
sudo ./gpio_test

# Continuous polling (1s interval, Ctrl+C to stop)
sudo ./gpio_test -l

# With raw SPI debug bytes
sudo ./gpio_test -d

# SPI loopback test (short MOSI pin 19 to MISO pin 21, no MCP3008)
sudo ./gpio_test --loopback
```

Daemon Testing

With the daemon running, submerge the water sensor into a cup of water covering the exposed traces on the PCB.

- The ADC reading will climb above the water threshold (default 100) and the daemon will log "Water detected".
- Removing the sensor from water will cause the reading to drop, logging "Pump cycle completed" and incrementing the cycle counter.
- To test alarm behavior, keep the sensor submerged for longer than `SUMP_ALARM_DELAY` (default 300s). The notification script will fire with the ALARM argument.

Query the socket directly to verify the 4-line output:
```Bash
echo "" | nc -U /tmp/sump_monitor.sock
```

Toggle debug output at runtime:
```Bash
sudo kill -USR1 $(pidof sump_monitord)
```

Note: Developed and tested on a Raspberry Pi Model B Rev 2

# Pictures

![Sensor_and_circuit](https://raw.githubusercontent.com/gshearer/sump_monitor/refs/heads/main/pics/sensor_and_circuit.jpg)
![Finished](https://raw.githubusercontent.com/gshearer/sump_monitor/refs/heads/main/pics/finished.jpg)
![Sump](https://raw.githubusercontent.com/gshearer/sump_monitor/refs/heads/main/pics/sump.jpg)
![Battery](https://raw.githubusercontent.com/gshearer/sump_monitor/refs/heads/main/pics/battery.jpg)
![Inverter_charger](https://raw.githubusercontent.com/gshearer/sump_monitor/refs/heads/main/pics/inverter_charger.png)
