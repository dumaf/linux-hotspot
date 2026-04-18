#!/bin/bash
# Linux Hotspot Installation Script
# Detects WiFi driver, installs dependencies, and sets up the systemd service

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVICE_NAME="hotspot.service"
SYSTEMD_DIR="/etc/systemd/system"
CONFIG_DIR="/etc/dnsmasq.d"
HOSTAPD_DIR="/etc/hostapd"

DETECTED_DRIVER=""

echo "========================================="
echo "Linux Hotspot Installation Script"
echo "========================================="

# Check if running as root
if [[ $EUID -ne 0 ]]; then
    echo "Error: This script must be run as root"
    exit 1
fi

# Function to detect WiFi driver
detect_wifi_driver() {
    echo ""
    echo "Detecting WiFi adapter and driver..."

    local wifi_device=""
    local driver=""

    # Try to find a WiFi device
    for dev in /sys/class/net/*; do
        local iface=$(basename "$dev")
        if [[ -d "/sys/class/net/$iface/wireless" ]]; then
            wifi_device="$iface"
            break
        fi
    done

    if [[ -z "$wifi_device" ]]; then
        # Try using ip link
        wifi_device=$(ip link show | grep -E '^[0-9]+: .*: wl' | head -1 | cut -d: -f2 | tr -d ' ')
    fi

    if [[ -z "$wifi_device" ]]; then
        echo "Warning: No WiFi interface detected!"
        return 1
    fi

    echo "Found WiFi interface: $wifi_device"

    # Get driver using ethtool or lspci
    if command -v ethtool &>/dev/null; then
        driver=$(ethtool -i "$wifi_device" 2>/dev/null | grep "driver:" | awk '{print $2}')
    fi

    if [[ -z "$driver" ]]; then
        # Try lspci
        driver=$(lspci -k 2>/dev/null | grep -A3 "$wifi_device" | grep "driver:" | awk '{print $2}')
    fi

    if [[ -z "$driver" ]]; then
        # Try reading from sysfs
        driver=$(readlink -f "/sys/class/net/$wifi_device/device/driver" 2>/dev/null | xargs basename)
    fi

    if [[ -n "$driver" ]]; then
        echo "Driver detected: $driver"
        DETECTED_DRIVER="$driver"

        # Check if driver supports AP mode
        if iw list 2>/dev/null | grep -A10 "Supported interface modes" | grep -q "AP"; then
            echo "Driver supports AP mode - Good!"
            return 0
        else
            echo "Warning: Driver may not support AP mode"
            echo "You may need to use a different driver or USB adapter"
            return 1
        fi
    else
        echo "Warning: Could not detect driver"
        return 1
    fi
}

# Function to install required packages
install_packages() {
    echo ""
    echo "Installing required packages..."

    local packages=("hostapd" "dnsmasq" "iw" "iptables" "ip" "sed" "grep" "awk" "coreutils")

    # Check for optional packages
    if ! command -v lspci &>/dev/null; then
        packages+=("pciutils")
    fi
    if ! command -v ethtool &>/dev/null; then
        packages+=("ethtool")
    fi

    local missing=()

    for pkg in "${packages[@]}"; do
        if ! dpkg -l "$pkg" 2>/dev/null | grep -q "^ii"; then
            missing+=("$pkg")
        fi
    done

    if [[ ${#missing[@]} -gt 0 ]]; then
        echo "Installing: ${missing[*]}"
        apt-get update
        apt-get install -y "${missing[@]}"
    else
        echo "All required packages already installed"
    fi
}

# Function to update hostapd.conf with detected driver
update_hostapd_driver() {
    if [[ -n "$DETECTED_DRIVER" ]]; then
        echo ""
        echo "Updating hostapd.conf with driver: $DETECTED_DRIVER"
        sed -i "s/^driver=.*/driver=$DETECTED_DRIVER/" "$SCRIPT_DIR/hostapd.conf"
        echo "Updated driver in $SCRIPT_DIR/hostapd.conf"
    else
        echo "No driver detected, keeping default in hostapd.conf"
    fi
}

# Function to copy configuration files
copy_config_files() {
    echo ""
    echo "Copying configuration files..."

    # Create directories if they don't exist
    mkdir -p "$CONFIG_DIR"
    mkdir -p "$HOSTAPD_DIR"

    # Copy dnsmasq config
    if [[ -f "$SCRIPT_DIR/dnsmasq.conf" ]]; then
        cp "$SCRIPT_DIR/dnsmasq.conf" "$CONFIG_DIR/hotspot.conf"
        echo "Copied dnsmasq.conf -> $CONFIG_DIR/hotspot.conf"
    else
        echo "Warning: dnsmasq.conf not found in $SCRIPT_DIR"
    fi

    # Copy hostapd config
    if [[ -f "$SCRIPT_DIR/hostapd.conf" ]]; then
        cp "$SCRIPT_DIR/hostapd.conf" "$HOSTAPD_DIR/hostapd.conf"
        echo "Copied hostapd.conf -> $HOSTAPD_DIR/hostapd.conf"
    else
        echo "Warning: hostapd.conf not found in $SCRIPT_DIR"
    fi
}

# Function to install systemd service
install_systemd_service() {
    echo ""
    echo "Installing systemd service..."

    # Check if the service file exists
    if [[ ! -f "$SCRIPT_DIR/$SERVICE_NAME" ]]; then
        echo "Error: $SERVICE_NAME not found in $SCRIPT_DIR"
        exit 1
    fi

    # Copy to systemd directory
    cp "$SCRIPT_DIR/$SERVICE_NAME" "$SYSTEMD_DIR/$SERVICE_NAME"
    echo "Copied $SERVICE_NAME -> $SYSTEMD_DIR/$SERVICE_NAME"

    # Update the service file with actual script path
    sed -i "s|%h/linux hotspot/hotspot.sh|$SCRIPT_DIR/hotspot.sh|g" "$SYSTEMD_DIR/$SERVICE_NAME"

    # Reload systemd
    systemctl daemon-reload
    echo "Reloaded systemd daemon"

    # Enable the service
    systemctl enable "$SERVICE_NAME"
    echo "Enabled $SERVICE_NAME"
}

# Main execution
main() {
    # Detect WiFi driver
    detect_wifi_driver || echo "Proceeding anyway..."

    # Update hostapd.conf with detected driver
    update_hostapd_driver

    # Install packages
    install_packages

    # Copy config files
    copy_config_files

    # Install systemd service
    install_systemd_service

    echo ""
    echo "========================================="
    echo "Installation complete!"
    echo "========================================="
    echo ""
    echo "To start the hotspot service:"
    echo "  sudo systemctl start hotspot"
    echo ""
    echo "To stop the hotspot service:"
    echo "  sudo systemctl stop hotspot"
    echo ""
    echo "To check status:"
    echo "  sudo systemctl status hotspot"
    echo ""
    echo "To enable on boot:"
    echo "  sudo systemctl enable hotspot"
}

main "$@"