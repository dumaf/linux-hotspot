#!/bin/bash
# Linux Hotspot Service Script
# This script sets up a WiFi hotspot using a virtual interface

# Load configuration
CONFIG_FILE="./hotspot.conf"
if [[ -f "$CONFIG_FILE" ]]; then
    source "$CONFIG_FILE"
else
    echo "Configuration file $CONFIG_FILE not found!"
    exit 1
fi

# Function to cleanup on exit or error
cleanup() {
    echo "Cleaning up..."
    # Remove virtual interface if it exists
    if ip link show "$HOTSPOT_INTERFACE" &>/dev/null; then
        ip link set "$HOTSPOT_INTERFACE" down
        iw dev "$HOTSPOT_INTERFACE" del
    fi
    # Stop hostapd and dnsmasq if they were started by this script
    pkill -f "hostapd.*$HOTSPOT_INTERFACE" 2>/dev/null
    pkill -f "dnsmasq.*$HOTSPOT_INTERFACE" 2>/dev/null
    # Remove iptables rules we added (optional, but clean)
    iptables -t nat -D POSTROUTING -o "$UPLINK_INTERFACE" -j MASQUERADE 2>/dev/null
    iptables -D FORWARD -i "$HOTSPOT_INTERFACE" -o "$UPLINK_INTERFACE" -j ACCEPT 2>/dev/null
    iptables -D FORWARD -i "$UPLINK_INTERFACE" -o "$HOTSPOT_INTERFACE" -m state --state RELATED,ESTABLISHED -j ACCEPT 2>/dev/null
    echo "Cleanup complete."
}

# Trap EXIT signal to cleanup
trap cleanup EXIT

# Check if we are root
if [[ $EUID -ne 0 ]]; then
   echo "This script must be run as root" 
   exit 1
fi

# Check required commands
for cmd in ip iw hostapd dnsmasq; do
    if ! command -v $cmd &>/dev/null; then
        echo "Error: $cmd is not installed. Please install it."
        exit 1
    fi
done

echo "Starting hotspot setup..."
echo "Uplink interface: $UPLINK_INTERFACE"
echo "Hotspot interface: $HOTSPOT_INTERFACE"
echo "Hotspot SSID: $HOTSPOT_SSID"

# Bring up uplink interface (assuming it's already configured and connected)
if ! ip link show "$UPLINK_INTERFACE" &>/dev/null; then
    echo "Error: Uplink interface $UPLINK_INTERFACE not found!"
    exit 1
fi

# Create virtual interface for AP
echo "Creating virtual interface $HOTSPOT_INTERFACE..."
# First, if the interface already exists (from a previous run), remove it
if ip link show "$HOTSPOT_INTERFACE" &>/dev/null; then
    ip link set "$HOTSPOT_INTERFACE" down
    iw dev "$HOTSPOT_INTERFACE" del
fi

# Add the virtual interface in AP mode
iw dev "$UPLINK_INTERFACE" interface add "$HOTSPOT_INTERFACE" type __ap
if [[ $? -ne 0 ]]; then
    echo "Failed to create virtual interface. Your WiFi driver may not support virtual AP mode."
    exit 1
fi

# Configure the hotspot interface with static IP
ip link set "$HOTSPOT_INTERFACE" up
ip addr add "$HOTSPOT_IP/$HOTSPOT_NETMASK" brd "$HOTSPOT_BROADCAST" dev "$HOTSPOT_INTERFACE"

# Start dnsmasq for DHCP and DNS
echo "Starting dnsmasq..."
cp ./dnsmasq.conf /etc/dnsmasq.d/hotspot.conf
# Customize the dnsmasq config with our variables
sed -i "s/^interface=.*/interface=$HOTSPOT_INTERFACE/" /etc/dnsmasq.d/hotspot.conf
sed -i "s/^dhcp-range=.*/dhcp-range=$HOTSPOT_DHCP_START,$HOTSPOT_DHCP_END,$HOTSPOT_DHCP_LEASETIME/" /etc/dnsmasq.d/hotspot.conf
sed -i "s/^dhcp-option=3,.*/dhcp-option=3,$HOTSPOT_IP/" /etc/dnsmasq.d/hotspot.conf
sed -i "s/^dhcp-option=6,.*/dhcp-option=6,$HOTSPOT_DNS1,$HOTSPOT_DNS2/" /etc/dnsmasq.d/hotspot.conf
sed -i "s/^server=.*/server=$HOTSPOT_DNS1/" /etc/dnsmasq.d/hotspot.conf
# Add second DNS line if not present
if ! grep -q "^server=$HOTSPOT_DNS2" /etc/dnsmasq.d/hotspot.conf; then
    echo "server=$HOTSPOT_DNS2" >> /etc/dnsmasq.d/hotspot.conf
fi

dnsmasq --conf-file=/etc/dnsmasq.d/hotspot.conf --no-daemon &
DNSMASQ_PID=$!
sleep 2  # Give dnsmasq time to start

# Start hostapd
echo "Starting hostapd..."
cp ./hostapd.conf /etc/hostapd/hostapd.conf
# Customize hostapd config
sed -i "s/^interface=.*/interface=$HOTSPOT_INTERFACE/" /etc/hostapd/hostapd.conf
sed -i "s/^ssid=.*/ssid=$HOTSPOT_SSID/" /etc/hostapd/hostapd.conf
sed -i "s/^passphrase=.*/passphrase=$HOTSPOT_PASSWORD/" /etc/hostapd/hostapd.conf

hostapd /etc/hostapd/hostapd.conf &
HOSTAPD_PID=$!
sleep 2  # Give hostapd time to start

# Enable IP forwarding
echo "Enabling IP forwarding..."
sysctl -w net.ipv4.ip_forward=1 >/dev/null

# Set up NAT and forwarding
echo "Setting up NAT and forwarding..."
iptables -t nat -A POSTROUTING -o "$UPLINK_INTERFACE" -j MASQUERADE
iptables -A FORWARD -i "$HOTSPOT_INTERFACE" -o "$UPLINK_INTERFACE" -j ACCEPT
iptables -A FORWARD -i "$UPLINK_INTERFACE" -o "$HOTSPOT_INTERFACE" -m state --state RELATED,ESTABLISHED -j ACCEPT

echo "Hotspot is now active!"
echo "SSID: $HOTSPOT_SSID"
echo "Password: $HOTSPOT_PASSWORD"
echo "Uplink interface: $UPLINK_INTERFACE (providing internet)"
echo "Hotspot interface: $HOTSPOT_INTERFACE (at $HOTSPOT_IP)"
echo "Press Ctrl+C to stop the hotspot."

# Wait for interrupt
while true; do
    sleep 1
done