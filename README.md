# 🌊 Sump Monitor

A high-performance, bare-metal **POSIX C** daemon designed for Raspberry Pi to monitor sump pit water levels. By utilizing the modern `libgpiod` v2 API, the monitor uses hardware interrupts for a near-zero CPU footprint. When water bridges the sensor pins, the system triggers a local audible alarm and executes a customizable notification script.

## 🛠 Hardware Setup

### Wiring Diagram

| Component | Raspberry Pi Pin | GPIO Number |
| --- | --- | --- |
| **Sensor Wire 1** | Pin 6 (GND) | Ground |
| **Sensor Wire 2** | Pin 11 | GPIO 17 |

### Power & Audio

* **Audio:** USB-powered speaker (e.g., Adafruit Mini USB Speaker).
* **Power:** To ensure monitoring during storms, it is recommended to use a battery backup system (e.g., 3000W Inverter/Charger with a 100AH LiFePO4 battery).

!

## 📦 Prerequisites

This project requires the latest **Raspberry Pi OS (Bookworm or newer)** to support the `libgpiod` v2 library. Ensure your system is fully patched before beginning.

```bash
sudo apt update && sudo apt dist-upgrade -y
sudo apt install -y git build-essential libgpiod-dev alsa-utils curl snmpd snmp

```

## 🚀 Installation

### 1. Clone and Compile

```bash
git clone https://github.com/gshearer/sump_monitor
cd sump_monitor
gcc -Wall -s -o sump_monitord sump_monitord.c -lgpiod

```

### 2. System Integration

Deploy the binaries, audio assets, and the systemd service file.

```bash
# Create sound directory and move assets
sudo mkdir -p /usr/local/share/sound
sudo cp assets/sound.wav /usr/local/share/sound/

# Install executables and service file
sudo cp sump_monitord sump_notify.sh /usr/local/bin/
sudo cp sump_monitor.service /etc/systemd/system/

```

### 3. Permissions & Security

These permissions ensure the notification script (containing API keys/passwords) and the daemon are only accessible by root.

```bash
sudo chown root:root /usr/local/bin/sump_notify.sh /usr/local/bin/sump_monitord /etc/systemd/system/sump_monitor.service
sudo chmod 0700 /usr/local/bin/sump_notify.sh
sudo chmod 0500 /usr/local/bin/sump_monitord
sudo chmod 0644 /etc/systemd/system/sump_monitor.service

```

## 🔔 Notification Configuration

Edit the notification script to configure your preferred alerts. The included example supports **ntfy.sh** for push notifications and **Gmail SMTP** via `curl`.

```bash
sudo nano /usr/local/bin/sump_notify.sh

```

*Note: If using Gmail, ensure you use a 16-character **App Password** generated from your Google Account security settings.*

## 📊 SNMP Integration

To monitor the sump status via an NMS (like LibreNMS or Zabbix), integrate the daemon's Unix socket into `snmpd`.

1. **Edit the SNMP Configuration:**
```bash
sudo nano /etc/snmp/snmpd.conf

```


2. **Add the Extension:**
```text
extend sumpStatus /usr/bin/bash -c '/bin/echo "" | /usr/bin/nc -U /tmp/sump_monitor.sock'

```


3. **Restart Services:**
```bash
sudo systemctl restart snmpd
sudo systemctl enable --now sump_monitor.service

```



## 🧪 Testing

You can verify the system is active by checking the status of the socket or by simulating a water event.

### Local Socket Test

```bash
echo "" | nc -U /tmp/sump_monitor.sock
# Returns 0 (Dry) or 1 (Wet)

```

### Hardware Test

Bridge **Pin 6** and **Pin 11** with a jumper wire or paperclip.

1. The local speaker should play the `sound.wav` alarm.
2. An email/push notification should be dispatched.
3. The `sumpStatus` SNMP OID will flip to `1`.

### Logs

To view the real-time execution logs of the daemon:

```bash
journalctl -u sump_monitor.service -f

```
