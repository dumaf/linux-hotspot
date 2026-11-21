# Linux Hotspot (Same-Channel Wi-Fi AP with Packet Engine)

Turn a Linux laptop into a **WPA2 Wi-Fi hotspot** that shares internet with your phone,
even when the laptop itself is connected via Wi-Fi (single radio, same-channel concurrency).

Includes a **Pure C packet monitoring engine** that captures and displays all traffic
flowing through the hotspot in real time.

## Features

- **Real Wi-Fi Network**: Phone connects with standard SSID + WPA2 password.
- **Same-Channel AP**: Auto-detects the laptop's active Wi-Fi channel and broadcasts the AP on the same frequency.
- **Transparent Gateway**: Phone needs zero proxy/VPN configuration. All traffic routes through the laptop automatically.
- **Wi-Fi QR Code**: Displays a scannable QR code in the terminal for instant one-tap connection.
- **Live Packet Monitor**: Pure C engine shows DNS queries, TCP/UDP flows, and bandwidth in real time.
- **Persistent Config**: SSID, password, and QR code remain stable across restarts.

## Quick Start

```bash
# Build the packet engine
make

# Start the hotspot (requires root)
sudo ./hotspot start
```

When it starts, it prints:
- Wi-Fi QR code (scan with phone camera)
- SSID and password
- Gateway IP and channel

Press `Ctrl+C` to stop.

## Commands

```bash
sudo ./hotspot start    # Start the hotspot
sudo ./hotspot stop     # Stop the hotspot
sudo ./hotspot status   # Show status and connected clients
sudo ./hotspot qr       # Re-display the Wi-Fi QR code
```

## Configuration

Edit `hotspot.conf` to customize:

```ini
SSID="LinuxHotspot"
PASSWORD="MySecurePassword123"
WIFI_IFACE=""              # Auto-detected if blank
SUBNET="192.168.12.1/24"
DNS1="1.1.1.1"
DNS2="8.8.8.8"
```

## Packet Engine

The C packet engine (`packet_engine`) monitors all phone traffic in real time:

```
[16:30:15] [DNS]  192.168.12.20 -> 1.1.1.1       Query: 'weather.com'
[16:30:16] [TCP]  192.168.12.20 -> 104.18.2.1:443 [HTTPS]  Len: 1250
[16:30:16] [TCP]  104.18.2.1:443 -> 192.168.12.20 [HTTPS]  Len: 4210
```

Build and test:

```bash
make          # Build packet_engine binary
make test     # Run automated test suite
make clean    # Clean build artifacts
```

## Requirements

- Linux with a Wi-Fi card supporting AP mode (check: `iw phy phy0 info | grep "* AP"`)
- `hostapd`, `dnsmasq`, `iw`, `iptables` (install: `sudo apt install hostapd dnsmasq iw iptables`)
- `gcc` and `make` for building the C packet engine

## Notes

- Your Wi-Fi hardware must support simultaneous AP + Station (client) mode.
- Intel wireless cards (iwlwifi) generally support this.
- The AP runs on the same channel as your existing Wi-Fi connection.
- The laptop's default internet route is used as the uplink for NAT.
