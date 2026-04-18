# Linux Hotspot Service

Turn your Linux machine into a WiFi hotspot using a virtual interface. This solution creates a software-based access point (AP) on your existing WiFi adapter, allowing you to share your internet connection or create a standalone wireless network with WPA2 security.

## Table of Contents
- [Overview](#overview)
- [Features](#features)
- [Requirements](#requirements)
- [Hardware Verification](#hardware-verification)
- [Installation](#installation)
- [Configuration](#configuration)
- [Usage](#usage)
  - [As a Systemd Service](#as-a-systemd-service)
  - [Manual Execution](#manual-execution)
- [How It Works](#how-it-works)
- [Troubleshooting](#troubleshooting)
- [Security Notes](#security-notes)
- [Customization](#customization)
- [License](#license)

## Overview

This project provides a complete solution to create a Linux WiFi hotspot that:
- Uses a virtual wireless interface (ap0) alongside your physical WiFi interface (wlan0)
- Offers WPA2-PSK security with customizable SSID and password
- Provides DHCP and DNS services for connected clients
- Enables NAT for internet sharing (when your uplink has internet access)
- Can be managed as a systemd service for easy startup/shutdown

## Features

- **Virtual Interface Creation**: Uses `iw` to create a virtual AP interface without needing additional hardware
- **WPA2 Security**: Strong encryption with customizable password (minimum 8 characters)
- **DHCP Server**: Automatically assigns IP addresses to clients via dnsmasq
- **DNS Forwarding**: Provides DNS resolution to clients
- **NAT/IP Forwarding**: Shares internet connection from your uplink interface
- **Systemd Integration**: Easy service management with start/stop/status commands
- **Configurable Network**: Adjustable IP range, gateway, and DNS settings
- **Logging**: Detailed logs via systemd journal

## Requirements

### Software
- Linux distribution with systemd (Ubuntu/Debian recommended)
- Required packages:
  - `hostapd` - WiFi access point daemon
  - `dnsmasq` - DHCP and DNS server
  - `iw` - Wireless configuration tool
  - `iproute2` - Network interface management (`ip` command)
  - `iptables` - Firewall for NAT rules

### Hardware
- WiFi adapter that supports **virtual interface mode** (AP mode)
- Most modern WiFi chips (Intel, Realtek, Atheros) with open-source drivers support this
- Minimum: 802.11g/n/ac adapter

## Hardware Verification

Before installing, verify your WiFi adapter supports virtual AP mode:

```bash
# Check if your device supports AP mode
iw list | grep -A 10 "Supported interface modes"
```

Look for `* AP` in the output. You should see something like:

```
Supported interface modes:
         * IBSS
         * managed
         * AP
         * AP/VLAN
         * WDS
         * monitor
         * mesh point
```

If you see `* AP`, your hardware supports creating a virtual access point.

Alternative check:
```bash
# Check specific device capabilities
iw phy phy0 info | grep -i ap
```

Replace `phy0` with your actual phy name if different (from `iw list` output).

## Installation

### 1. Install Dependencies

On Ubuntu/Debian:
```bash
sudo apt update
sudo apt install hostapd dnsmasq iw iproute2 iptables
```

On other distributions, use the appropriate package manager (dnf, pacman, etc.).

### 2. Get the Code

```bash
# Clone the repository (if you have a git repo)
# OR copy the files manually to a directory:
mkdir -p ~/linux-hotspot
cd ~/linux-hotspot
# Copy all the provided files here: hotspot.conf, hostapd.conf, dnsmasq.conf, hotspot.sh, hotspot.service
```

### 3. Make Script Executable

```bash
chmod +x hotspot.sh
```

## Configuration

Edit `hotspot.conf` to match your setup:

```bash
# Uplink interface (connected to internet - usually your WiFi)
UPLINK_INTERFACE=wlan0

# Hotspot interface (virtual AP that will be created)
HOTSPOT_INTERFACE=ap0

# Hotspot SSID and password
HOTSPOT_SSID=MyHotspot
HOTSPOT_PASSWORD=MySecurePassword  # Minimum 8 characters

# Hotspot network settings
HOTSPOT_IP=10.0.0.1
HOTSPOT_NETMASK=255.255.255.0
HOTSPOT_BROADCAST=10.0.0.255

# DHCP range for hotspot clients
HOTSPOT_DHCP_START=10.0.0.10
HOTSPOT_DHCP_END=10.0.0.50
HOTSPOT_DHCP_LEASETIME=12h

# DNS servers to provide to clients
HOTSPOT_DNS1=8.8.8.8
HOTSPOT_DNS2=8.8.4.4
```

**Important Notes:**
- `UPLINK_INTERFACE` should be your physical WiFi interface (check with `ip link show` or `iwconfig`)
- The hotspot will be created on `HOTSPOT_INTERFACE` (ap0 by default)
- Ensure `HOTSPOT_PASSWORD` is at least 8 characters for WPA2
- The hotspot network (`10.0.0.0/24` by default) should not conflict with your uplink network

## Usage

### As a Systemd Service (Recommended)

```bash
# Install the service
sudo cp hotspot.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable hotspot.service

# Start the hotspot
sudo systemctl start hotspot.service

# Check status
sudo systemctl status hotspot.service

# View logs
sudo journalctl -u hotspot.service -f

# Stop the hotspot
sudo systemctl stop hotspot.service

# Disable auto-start
sudo systemctl disable hotspot.service
```

### Manual Execution

```bash
# Start hotspot (runs in foreground)
sudo ./hotspot.sh

# Press Ctrl+C to stop when running manually
```

## How It Works

1. **Virtual Interface Creation**: The script uses `iw dev $UPLINK_INTERFACE interface add $HOTSPOT_INTERFACE type __ap` to create a virtual wireless interface in AP mode.

2. **Network Configuration**: Assigns a static IP (`$HOTSPOT_IP/$HOTSPOT_NETMASK`) to the virtual interface.

3. **DHCP & DNS**: Starts dnsmasq to provide IP addresses (via DHCP) and DNS resolution to clients.

4. **Access Point**: Starts hostapd to broadcast the SSID with WPA2 security.

5. **Internet Sharing**: 
   - Enables IP forwarding (`net.ipv4.ip_forward=1`)
   - Sets up NAT with iptables: `iptables -t nat -A POSTROUTING -o $UPLINK_INTERFACE -j MASQUERADE`
   - Configures forwarding rules to allow traffic between hotspot and uplink interfaces.

6. **Client Connection**: Devices connect to the hotspot SSID, receive an IP via DHCP, and can access the internet (if uplink has connectivity) or communicate on the local network.

## Troubleshooting

### Common Issues

1. **"Failed to create virtual interface"**
   - Your WiFi driver doesn't support virtual AP mode
   - Solution: Try a different driver or use a USB WiFi adapter known to support AP mode

2. **Clients can't get IP addresses**
   - Check dnsmasq is running: `sudo systemctl status dnsmasq` or check journal
   - Ensure no other DHCP server is conflicting on the same network

3. **No internet access on clients**
   - Verify uplink interface has internet: `ping 8.8.8.8`
   - Check IP forwarding: `cat /proc/sys/net/ipv4/ip_forward` (should be 1)
   - Verify NAT rule: `sudo iptables -t nat -L -v -n | grep MASQUERADE`

4. **Hotspot not visible**
   - Check hostapd logs: `sudo journalctl -u hotspot.service | grep hostapd`
   - Ensure regulatory domain allows your channel: `iw reg get`

5. **Authentication failures**
   - Double-check password length (≥8 chars)
   - Verify WPA2 settings in hostapd.conf

### Logs & Debugging

```bash
# Follow logs in real-time
sudo journalctl -u hotspot.service -f

# Check if interfaces were created
ip link show
iw dev

# Test manual execution for more verbose output
sudo ./hotspot.sh
```

## Security Notes

- WPA2-PSK provides strong encryption when using a complex password
- Change the default SSID and password immediately
- The hotspot isolates clients from each other only by default Linux behavior; for client isolation, additional ebtables/iptables rules would be needed
- Do not leave the hotspot running unattended in public places without proper security
- Consider implementing a captive portal or additional authentication for public hotspots

## Customization

### Changing Network Range
Modify in `hotspot.conf`:
- `HOTSPOT_IP`, `HOTSPOT_NETMASK`, `HOTSPOT_BROADCAST`
- `HOTSPOT_DHCP_START`, `HOTSPOT_DHCP_END`

### Different DNS Servers
Change `HOTSPOT_DNS1` and `HOTSPOT_DNS2` in `hotspot.conf`

### Static Clients
Edit the generated dnsmasq.conf to add:
```
dhcp-host=AA:BB:CC:DD:EE:FF,10.0.0.100
```

### 5GHz Band
Change in hostapd.conf:
```
hw_mode=a
channel=36
```

## License

This project is provided as-is for educational purposes. You are free to modify and use it according to your needs.

## Acknowledgements

- Based on standard Linux wireless tools: hostapd, dnsmasq, iw
- Inspired by various DIY hotspot tutorials and scripts