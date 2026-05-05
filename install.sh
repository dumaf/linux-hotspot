#!/usr/bin/env bash
# Install dependencies and systemd wiring for linux-hotspot.

set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SYSTEMD_DIR="/etc/systemd/system"
SERVICE_NAME="hotspot.service"
SCRIPT_TARGET="/usr/local/bin/linux-hotspot"

log() {
    printf '[install] %s\n' "$*"
}

die() {
    printf '[install] ERROR: %s\n' "$*" >&2
    exit 1
}

require_root() {
    if [[ $EUID -eq 0 ]]; then
        return 0
    fi
    if command -v sudo >/dev/null 2>&1; then
        exec sudo -E bash "$0" "$@"
    fi
    die "Run this installer as root."
}

detect_pm() {
    if command -v apt-get >/dev/null 2>&1; then
        echo "apt"
        return
    fi
    if command -v dnf >/dev/null 2>&1; then
        echo "dnf"
        return
    fi
    if command -v pacman >/dev/null 2>&1; then
        echo "pacman"
        return
    fi
    if command -v zypper >/dev/null 2>&1; then
        echo "zypper"
        return
    fi
    echo ""
}

install_packages() {
    local pm
    pm="$(detect_pm)"
    if [[ -z "$pm" ]]; then
        die "Unsupported distro package manager. Install hostapd dnsmasq iw iproute2 iptables manually."
    fi

    log "Installing dependencies via $pm..."
    case "$pm" in
        apt)
            apt-get update
            apt-get install -y hostapd dnsmasq iw iproute2 iptables procps
            ;;
        dnf)
            dnf install -y hostapd dnsmasq iw iproute iptables procps-ng
            ;;
        pacman)
            pacman -Sy --noconfirm hostapd dnsmasq iw iproute2 iptables procps-ng
            ;;
        zypper)
            zypper --non-interactive install hostapd dnsmasq iw iproute2 iptables procps
            ;;
        *)
            die "Unsupported package manager: $pm"
            ;;
    esac
}

install_script() {
    log "Installing hotspot runner to $SCRIPT_TARGET"
    install -m 0755 "$SCRIPT_DIR/hotspot.sh" "$SCRIPT_TARGET"
}

install_service() {
    log "Installing systemd service..."
    install -m 0644 "$SCRIPT_DIR/$SERVICE_NAME" "$SYSTEMD_DIR/$SERVICE_NAME"
    systemctl daemon-reload
    systemctl enable "$SERVICE_NAME"
}

main() {
    require_root "$@"
    install_packages
    install_script
    install_service

    echo
    echo "Installation complete."
    echo "Start hotspot now:   sudo systemctl start hotspot"
    echo "Check status:        sudo systemctl status hotspot"
    echo "Stop hotspot:        sudo systemctl stop hotspot"
    echo "Run manually:        sudo /usr/local/bin/linux-hotspot run"
}

main "$@"
