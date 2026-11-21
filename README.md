# Linux Hotspot

> **Single-radio, same-channel Wi-Fi Access Point with an integrated Pure C packet monitoring engine and in-terminal QR code generator.**

Turn a Linux laptop into a **WPA2 Wi-Fi hotspot** that shares its internet connection with phones and other devices, even when the laptop itself is connected to upstream internet over Wi-Fi (single-radio AP + STA concurrency).

Includes a lightweight **Pure C packet monitoring engine** that captures and inspects traffic in real time using Linux raw sockets with zero third-party library dependencies.

---

## Features

- **Same-Channel Concurrency**: Auto-detects the active Wi-Fi channel and frequency of the upstream connection and provisions the AP on the same frequency (works on single Wi-Fi chips like Intel `iwlwifi`).
- **Terminal Wi-Fi QR Code**: Generates an ANSI half-block QR code directly in the terminal for instant, one-tap mobile connection without typing passwords.
- **Zero-Dependency C Packet Engine**: Captures traffic on the AP interface using `AF_PACKET` raw sockets. Dissects DNS queries, TCP flags/ports, UDP flows, ICMP, and directional throughput (Phone $\leftrightarrow$ Internet).
- **Periodic Live Dashboard**: Displays real-time metrics including total packet count, ingress/egress bytes, protocol breakdowns, and active flows.
- **Transparent Gateway**: Automatic IPv4 forwarding and `iptables` NAT masquerading. Connected devices require no manual proxy or VPN setup.
- **Robust Lifecycle Management**: Clean signal handling (`SIGINT` / `Ctrl+C`, `SIGTERM`) safely restores firewall rules, terminates child daemons, and removes virtual AP interfaces.
- **Self-Contained Configuration**: All network parameters (SSID, password, subnet, DHCP range, DNS resolvers) persist cleanly in `hotspot.conf`.

---

## Project Structure

```
.
├── Makefile                  # Build and test orchestration
├── README.md                 # Project documentation
├── detect_wifi.sh            # Wi-Fi hardware & capability detection script
├── hotspot                   # Primary hotspot lifecycle manager (bash)
├── hotspot.conf              # Persistent user configuration
├── packet_engine             # Compiled packet monitor & QR generator binary
├── src/
│   ├── main.c                # CLI entry point, argument parsing & stats dashboard
│   ├── packet_engine.c       # Raw socket capture, IP/TCP/UDP/DNS parsing & metrics
│   ├── packet_engine.h       # Packet engine header and public data structures
│   ├── qr.c                  # Self-contained ANSI terminal QR code generator
│   └── qr.h                  # QR generation interface
└── tests/
    └── test_packet_engine.c  # Automated unit test suite (QR & packet parsing)
```

---

## Requirements

### Supported Hardware
- Wi-Fi card with support for Access Point (AP) mode and simultaneous AP + Station (STA) concurrency (common on modern Intel, Qualcomm, and Atheros wireless chipsets).
- Verify AP support on your system:
  ```bash
  iw phy $(iw dev | grep -oP 'phy#[0-9]+' | head -1 | tr -d '#') info | grep "\* AP"
  ```

### Software Dependencies
- **Linux utilities**: `hostapd`, `dnsmasq`, `iw`, `iptables`, `iproute2`
- **Build tools**: `gcc`, `make` (standard C99 and POSIX threads)

#### Ubuntu / Debian
```bash
sudo apt update
sudo apt install -y hostapd dnsmasq iw iptables build-essential
```

#### Arch Linux
```bash
sudo pacman -S --needed hostapd dnsmasq iw iptables base-devel
```

#### Fedora / RHEL
```bash
sudo dnf install -y hostapd dnsmasq iw iptables gcc make
```

---

## Quick Start

### 1. Build the Packet Engine
```bash
make
```

To run the automated verification test suite:
```bash
make test
```

### 2. Start the Hotspot
Run the management script with root privileges:
```bash
sudo ./hotspot start
```

Upon startup, the console displays:
1. Scannable Wi-Fi QR code
2. SSID and WPA2 security credentials
3. Gateway IP address, channel, and interface details
4. Real-time packet activity and periodic traffic dashboards

Press `Ctrl+C` to cleanly stop the hotspot and restore network settings.

---

## Command Reference

The `hotspot` manager script supports the following subcommands:

```bash
sudo ./hotspot start    # Launch the AP, configure routing, and start monitor
sudo ./hotspot stop     # Stop services, flush iptables rules, remove virtual AP
sudo ./hotspot status   # Display running status, gateway IP, and connected clients
sudo ./hotspot detect   # Inspect Wi-Fi hardware, driver, AP capabilities & channel
sudo ./hotspot qr       # Display the connection QR code in terminal on demand
```

---

## Configuration (`hotspot.conf`)

Settings can be customized in `hotspot.conf`. Defaults are used if values are left unset:

```ini
# Hotspot Wi-Fi Credentials
SSID="LinuxHotspot"
PASSWORD="MySecurePassword123"

# Wi-Fi interface (leave blank to auto-detect active wireless adapter)
WIFI_IFACE=""

# Subnet and DHCP Allocation
SUBNET="192.168.12.1/24"
DHCP_START="192.168.12.20"
DHCP_END="192.168.12.250"

# Upstream DNS servers pushed to clients
DNS1="1.1.1.1"
DNS2="8.8.8.8"
```

---

## Wi-Fi Card Identification (`detect_wifi.sh`)

The integrated [`detect_wifi.sh`](detect_wifi.sh) script automatically identifies all wireless adapters on your system and selects the optimal one for hotspot broadcasting:

```bash
# Run standalone hardware inspection
./detect_wifi.sh

# Or via the hotspot manager
sudo ./hotspot detect
```

### Capabilities
- **Hardware & Driver Discovery**: Discovers physical PCI/USB adapters (e.g. Intel Wi-Fi 6 AX201), kernel drivers (`iwlwifi`, `ath9k`, `mt7921e`, etc.), and MAC addresses.
- **AP & Concurrency Checking**: Verifies whether each card supports Access Point (`* AP`) mode and simultaneous AP + Station concurrency.
- **Smart Card Selection**: Auto-selects the best adapter for the hotspot (preferring AP-capable and actively connected adapters).
- **Auto Channel Sync**: Detects the upstream network frequency and channel to allow same-channel operation on a single radio.
- **Scriptable Modes**:
  - `./detect_wifi.sh --get-iface`: Outputs only the selected interface name (e.g. `wlan0`).
  - `./detect_wifi.sh --export`: Emits bash environment variables for integration into other scripts.

---

## Standalone Packet Engine

The packet monitoring binary (`packet_engine`) can also be executed independently for inspection or debugging:

```bash
# Capture traffic on a specific interface and subnet
sudo ./packet_engine --iface ap0 --subnet 192.168.12 --stats-interval 5

# Generate only a Wi-Fi connection QR code and exit
./packet_engine --qr-only --ssid "MyHotspot" --password "SecretKey123"
```

### Live Traffic Output Example

```
[16:30:15] [DNS]  192.168.12.20 -> 1.1.1.1       Query: 'weather.com'
[16:30:16] [TCP]  192.168.12.20 -> 104.18.2.1:443 [HTTPS]  Len: 1250
[16:30:16] [TCP]  104.18.2.1:443 -> 192.168.12.20 [HTTPS]  Len: 4210

── Dashboard [16:30:20] ──────────────────────────
  Packets:     142
  Bytes In:    184520 (internet → phone)
  Bytes Out:   32140 (phone → internet)
  TCP:         118  UDP: 24  DNS: 8  ICMP: 0
───────────────────────────────────────────────
```

---

## Build Targets

The included `Makefile` provides the following targets:

```bash
make        # Compile the packet_engine binary
make test   # Compile and run unit test suite (tests/test_packet_engine.c)
make clean  # Remove build artifacts and compiled binaries
```

---

## Troubleshooting

- **Virtual AP interface creation fails (`type __ap`)**:
  Ensure your Wi-Fi device is not soft-blocked or hard-blocked:
  ```bash
  rfkill list wifi
  sudo rfkill unblock wifi
  ```
  Check whether your Wi-Fi card driver supports concurrent virtual interfaces:
  ```bash
  iw list | grep -A 10 "valid interface combinations"
  ```
- **Port 53 conflicts (DNS)**:
  `hotspot` specifies `bind-interfaces` so `dnsmasq` binds strictly to the virtual AP interface (`ap0`), avoiding conflicts with local resolvers like `systemd-resolved`.
- **Channel mismatch**:
  The AP automatically locks to the channel of your connected Wi-Fi station. If the upstream Wi-Fi roams or changes channel, restarting the hotspot (`sudo ./hotspot stop && sudo ./hotspot start`) re-syncs the AP frequency.
