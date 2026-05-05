# Linux Hotspot (Auto-Detect, One Command)

This project turns a Linux machine into a WPA2 hotspot with **no required config editing**.

The script auto-detects:
- uplink interface (default route)
- wireless interface to host AP
- free AP interface name (`ap0`, `ap1`, ...)
- non-conflicting `/24` hotspot subnet
- DNS servers from `/etc/resolv.conf`

## Quick Start

Run one command:

```bash
chmod +x hotspot.sh install.sh
sudo ./hotspot.sh
```

When it starts, it prints:
- SSID
- password
- hotspot gateway IP

Press `Ctrl+C` to stop.

## Optional Overrides

You can still override values when needed:

```bash
sudo ./hotspot.sh run \
  --ssid MyNet \
  --password SuperSecure123 \
  --uplink eth0 \
  --wifi wlan0 \
  --channel 6
```

Useful commands:

```bash
sudo ./hotspot.sh status
sudo ./hotspot.sh stop
```

## Installer (Systemd + Dependencies)

Install dependencies and register service:

```bash
sudo ./install.sh
```

This installs:
- runtime script at `/usr/local/bin/linux-hotspot`
- service at `/etc/systemd/system/hotspot.service`

Then use:

```bash
sudo systemctl start hotspot
sudo systemctl status hotspot
sudo systemctl stop hotspot
```

## What Was Fixed

Compared to the original scripts:
- removed manual `hotspot.conf` dependency for normal use
- fixed `hostapd` key field (`wpa_passphrase`, not `passphrase`)
- fixed invalid `ip addr add` usage with dotted netmask
- separated AP radio interface from NAT uplink interface logic
- replaced broad `pkill` cleanup with PID/state-based cleanup
- avoided mutating global config files on each run

## Notes

- Hardware/driver must support AP mode (`iw phy ...` with `* AP`).
- Some adapters cannot do AP + client mode simultaneously on one radio.
- Script currently uses `iptables` for NAT.
