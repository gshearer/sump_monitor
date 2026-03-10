🌊 Sump Monitor

A high-performance, bare-metal POSIX C daemon designed for Raspberry Pi to monitor sump pit water levels.

Utilizing the modern libgpiod v2 API, the monitor uses hardware polling and a 10-second software debouncing state machine to completely eliminate false positives from water turbulence and EMI. When a confirmed water event is detected, the system triggers a local audible alarm, executes a customizable notification script with state arguments (WET or DRY), and tracks total transitions.

It also features a Unix domain socket for seamless integration with NMS systems (like LibreNMS or Zabbix) via Net-SNMP.
🛠 Hardware Setup
Wiring Diagram
Component	Raspberry Pi Pin	GPIO Number
Sensor Wire 1	Pin 6 (GND)	Ground
Sensor Wire 2	Pin 11	GPIO 17
Power & Audio

    Audio: USB-powered speaker (e.g., Adafruit Mini USB Speaker).

    Power: To ensure monitoring during severe storms, use a battery backup system.

(Add your images here)
📦 Prerequisites

This project requires the latest Raspberry Pi OS (Bookworm or newer) to support the libgpiod v2 library. Ensure your system is fully patched before beginning.
Bash

sudo apt update && sudo apt dist-upgrade -y
sudo apt install -y git build-essential libgpiod-dev alsa-utils curl snmpd snmp

🚀 Installation
1. Clone and Compile

Clone the repository and compile the daemon linking the modern GPIO library.
Bash

git clone https://github.com/gshearer/sump_monitor
cd sump_monitor
gcc -Wall -s -o sump_monitord sump_monitord.c -lgpiod

2. System Integration

Deploy the binaries, audio assets, and the systemd service file into the standard Linux directories.
Bash

# Create sound directory and move sound file
sudo mkdir -p /usr/local/share/sound
sudo mv sump_alert.wav /usr/local/share/sound/

# Install executables and service file
1. Edit sound_notify.sh, update alsa commands with your sound device (usb speaker, headphone jack, etc)
2. Update with yoru ntfy.sh subject or some other alerting mechanism

sudo mv sump_monitord sump_notify.sh /usr/local/bin/
sudo mv sump_monitor.service /etc/systemd/system/

3. Permissions & Security

Lock down the daemon and the notification script (which may contain API keys) so they are only accessible by root.
Bash

sudo chown root:root /usr/local/bin/sump_notify.sh /usr/local/bin/sump_monitord /etc/systemd/system/sump_monitor.service /usr/local/share/sound/sump_alert.wav
sudo chmod 0700 /usr/local/bin/sump_notify.sh
sudo chmod 0500 /usr/local/bin/sump_monitord
sudo chmod 0644 /etc/systemd/system/sump_monitor.service /usr/local/share/sound/sump_alert.wav

📊 SNMP Integration

The daemon's Unix socket outputs two lines of data: Line 1 is the current Boolean state (0 or 1), and Line 2 is the cumulative transition counter.

    Add the Extension to /etc/snmp/snmpd.conf:
    Plaintext

    extend sumpMetrics /usr/bin/bash -c '/bin/echo "" | /usr/bin/nc -U /tmp/sump_monitor.sock'

    Restart Services:
    Bash

    sudo systemctl enable --now snmpd.service
    sudo systemctl enable --now sump_monitor.service

    Query the OIDs from your NMS:

        Current State: snmpget -v2c -c public <IP> 'NET-SNMP-EXTEND-MIB::nsExtendOutLine."sumpMetrics".1'

        Event Counter: snmpget -v2c -c public <IP> 'NET-SNMP-EXTEND-MIB::nsExtendOutLine."sumpMetrics".2'

🧪 Testing

Bridge Pin 6 and Pin 11 with a jumper wire. Hold it for 10 seconds to satisfy the software debounce.

    The local speaker will play the sound.wav alarm.

    The WET notification will be dispatched.

    The sumpMetrics SNMP state will flip to 1, and the counter will increment.

Removing the jumper wire for 10 seconds will dispatch the DRY notification and reset the SNMP state to 0.

Or just unplug your sump pump and let the water rise to your sensor wires connected to the gpio pins :-)

Note: I used a Raspberry Pie Model B Rev 2

# Pictures

![RPI pins](https://raw.githubusercontent.com/gshearer/sump_monitor/refs/heads/main/pics/rpi_pins.jpg)
![RPI_with_case](https://raw.githubusercontent.com/gshearer/sump_monitor/refs/heads/main/pics/rpi_with_case.jpg)
![RPI with_sensorwire](https://raw.githubusercontent.com/gshearer/sump_monitor/refs/heads/main/pics/rpi_with_sensorwire.jpg)
![sump_with_sensor](https://raw.githubusercontent.com/gshearer/sump_monitor/refs/heads/main/pics/sump_with_sensor.jpg)
![battery](https://raw.githubusercontent.com/gshearer/sump_monitor/refs/heads/main/pics/battery.jpg)
![inverter_charger](https://raw.githubusercontent.com/gshearer/sump_monitor/refs/heads/main/pics/inverter_charger.png)
