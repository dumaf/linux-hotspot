#!/usr/bin/env bash
# Single-command Linux hotspot runner with auto-detection.
# Run: sudo ./hotspot.sh

set -Eeuo pipefail

SCRIPT_NAME="$(basename "$0")"
STATE_DIR="/run/linux-hotspot"
STATE_FILE="$STATE_DIR/state.env"
DNSMASQ_CONF="$STATE_DIR/dnsmasq.conf"
HOSTAPD_CONF="$STATE_DIR/hostapd.conf"
DNSMASQ_PID_FILE="$STATE_DIR/dnsmasq.pid"
HOSTAPD_PID_FILE="$STATE_DIR/hostapd.pid"

ACTION="run"
HOTSPOT_SSID="${HOTSPOT_SSID:-}"
HOTSPOT_PASSWORD="${HOTSPOT_PASSWORD:-}"
HOTSPOT_CHANNEL="${HOTSPOT_CHANNEL:-6}"
UPLINK_INTERFACE="${UPLINK_INTERFACE:-}"
WIFI_INTERFACE="${WIFI_INTERFACE:-}"
HOTSPOT_INTERFACE="${HOTSPOT_INTERFACE:-}"
SUBNET_PREFIX="${SUBNET_PREFIX:-}"
DHCP_LEASETIME="${DHCP_LEASETIME:-12h}"
HOTSPOT_DNS1="${HOTSPOT_DNS1:-}"
HOTSPOT_DNS2="${HOTSPOT_DNS2:-}"

HOTSPOT_IP=""
HOTSPOT_DHCP_START=""
HOTSPOT_DHCP_END=""
ORIGINAL_IP_FORWARD=""
RUNNING_DNSMASQ_PID=""
RUNNING_HOSTAPD_PID=""

log() {
    printf '[hotspot] %s\n' "$*"
}

warn() {
    printf '[hotspot] WARNING: %s\n' "$*" >&2
}

die() {
    printf '[hotspot] ERROR: %s\n' "$*" >&2
    exit 1
}

usage() {
    cat <<EOF
Usage:
  $SCRIPT_NAME [run|start] [options]
  $SCRIPT_NAME stop
  $SCRIPT_NAME status

Options:
  --ssid NAME             Hotspot SSID (default: LinuxHotspot-<hostname>)
  --password PASS         WPA2 passphrase (8..63 chars, auto-generated if empty)
  --channel N             WiFi channel (default: 6)
  --uplink IFACE          Interface used for internet egress (default: default route)
  --wifi IFACE            Physical wireless interface to create AP from (default: auto)
  --ap IFACE              AP virtual interface name (default: first free apN)
  --subnet-prefix A.B.C   Prefix for hotspot /24 (default: auto-picked non-conflicting)
  --dns1 IP               Primary DNS for clients (default: from /etc/resolv.conf or 1.1.1.1)
  --dns2 IP               Secondary DNS for clients (default: from /etc/resolv.conf or 8.8.8.8)
  --lease TIME            DHCP lease time (default: 12h)
  -h, --help              Show this help

Environment variables can also be used:
  HOTSPOT_SSID HOTSPOT_PASSWORD HOTSPOT_CHANNEL UPLINK_INTERFACE WIFI_INTERFACE
  HOTSPOT_INTERFACE SUBNET_PREFIX HOTSPOT_DNS1 HOTSPOT_DNS2 DHCP_LEASETIME
EOF
}

command_exists() {
    command -v "$1" >/dev/null 2>&1
}

require_root() {
    if [[ $EUID -eq 0 ]]; then
        return 0
    fi

    if command_exists sudo; then
        exec sudo -E bash "$0" "$@"
    fi

    die "This script must be run as root (or with sudo installed)."
}

detect_package_manager() {
    if command_exists apt-get; then
        echo "apt"
        return 0
    fi
    if command_exists dnf; then
        echo "dnf"
        return 0
    fi
    if command_exists pacman; then
        echo "pacman"
        return 0
    fi
    if command_exists zypper; then
        echo "zypper"
        return 0
    fi
    echo ""
}

install_dependencies_if_needed() {
    local required=(ip iw hostapd dnsmasq iptables sysctl awk sed grep tr head)
    local missing=()
    local cmd

    for cmd in "${required[@]}"; do
        if ! command_exists "$cmd"; then
            missing+=("$cmd")
        fi
    done

    if [[ ${#missing[@]} -eq 0 ]]; then
        return 0
    fi

    log "Missing commands: ${missing[*]}"

    local pm
    pm="$(detect_package_manager)"
    if [[ -z "$pm" ]]; then
        die "No supported package manager found. Install: hostapd dnsmasq iw iproute2 iptables."
    fi

    log "Installing dependencies with $pm..."
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

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            run|start)
                ACTION="run"
                shift
                ;;
            stop)
                ACTION="stop"
                shift
                ;;
            status)
                ACTION="status"
                shift
                ;;
            --ssid)
                [[ $# -ge 2 ]] || die "--ssid requires a value"
                HOTSPOT_SSID="${2:-}"
                shift 2
                ;;
            --password)
                [[ $# -ge 2 ]] || die "--password requires a value"
                HOTSPOT_PASSWORD="${2:-}"
                shift 2
                ;;
            --channel)
                [[ $# -ge 2 ]] || die "--channel requires a value"
                HOTSPOT_CHANNEL="${2:-}"
                shift 2
                ;;
            --uplink)
                [[ $# -ge 2 ]] || die "--uplink requires a value"
                UPLINK_INTERFACE="${2:-}"
                shift 2
                ;;
            --wifi)
                [[ $# -ge 2 ]] || die "--wifi requires a value"
                WIFI_INTERFACE="${2:-}"
                shift 2
                ;;
            --ap)
                [[ $# -ge 2 ]] || die "--ap requires a value"
                HOTSPOT_INTERFACE="${2:-}"
                shift 2
                ;;
            --subnet-prefix)
                [[ $# -ge 2 ]] || die "--subnet-prefix requires a value"
                SUBNET_PREFIX="${2:-}"
                shift 2
                ;;
            --dns1)
                [[ $# -ge 2 ]] || die "--dns1 requires a value"
                HOTSPOT_DNS1="${2:-}"
                shift 2
                ;;
            --dns2)
                [[ $# -ge 2 ]] || die "--dns2 requires a value"
                HOTSPOT_DNS2="${2:-}"
                shift 2
                ;;
            --lease)
                [[ $# -ge 2 ]] || die "--lease requires a value"
                DHCP_LEASETIME="${2:-}"
                shift 2
                ;;
            -h|--help)
                usage
                exit 0
                ;;
            *)
                die "Unknown argument: $1 (use --help)"
                ;;
        esac
    done
}

is_wireless_interface() {
    local iface="$1"
    [[ -d "/sys/class/net/$iface/wireless" ]]
}

default_uplink_interface() {
    ip route show default 2>/dev/null | awk 'NR==1 {for(i=1;i<=NF;i++) if ($i=="dev") {print $(i+1); exit}}'
}

pick_wireless_interface() {
    local iface

    if [[ -n "$WIFI_INTERFACE" ]]; then
        if ! ip link show "$WIFI_INTERFACE" >/dev/null 2>&1; then
            die "Requested WiFi interface '$WIFI_INTERFACE' does not exist."
        fi
        if ! is_wireless_interface "$WIFI_INTERFACE"; then
            die "Requested WiFi interface '$WIFI_INTERFACE' is not wireless."
        fi
        return 0
    fi

    if [[ -n "$UPLINK_INTERFACE" ]] && is_wireless_interface "$UPLINK_INTERFACE"; then
        WIFI_INTERFACE="$UPLINK_INTERFACE"
        return 0
    fi

    while read -r iface; do
        if [[ -n "$iface" ]] && is_wireless_interface "$iface"; then
            WIFI_INTERFACE="$iface"
            return 0
        fi
    done < <(iw dev 2>/dev/null | awk '$1=="Interface" {print $2}')

    while read -r iface; do
        if [[ -n "$iface" ]] && is_wireless_interface "$iface"; then
            WIFI_INTERFACE="$iface"
            return 0
        fi
    done < <(ls /sys/class/net 2>/dev/null)

    die "No wireless interface found."
}

pick_ap_interface_name() {
    local candidate

    if [[ -n "$HOTSPOT_INTERFACE" ]]; then
        return 0
    fi

    for candidate in ap0 ap1 ap2 wlan-ap0; do
        if ! ip link show "$candidate" >/dev/null 2>&1; then
            HOTSPOT_INTERFACE="$candidate"
            return 0
        fi
    done

    die "Could not find a free AP interface name."
}

ensure_ap_support() {
    local phy
    phy="$(iw dev "$WIFI_INTERFACE" info 2>/dev/null | awk '/wiphy/ {print "phy"$2; exit}')"
    if [[ -z "$phy" ]]; then
        die "Could not map interface '$WIFI_INTERFACE' to a wireless phy."
    fi

    if ! iw phy "$phy" info 2>/dev/null | grep -qE '^[[:space:]]+\*[[:space:]]+AP$'; then
        die "Wireless phy '$phy' does not report AP mode support."
    fi
}

pick_subnet_prefix() {
    local candidates=(
        "10.88.0"
        "10.89.0"
        "10.90.0"
        "10.91.0"
        "172.29.250"
        "192.168.77"
    )
    local routes
    local prefix
    routes="$(ip -4 route show 2>/dev/null || true)"

    if [[ -n "$SUBNET_PREFIX" ]]; then
        if ! [[ "$SUBNET_PREFIX" =~ ^([0-9]{1,3}\.){2}[0-9]{1,3}$ ]]; then
            die "Invalid --subnet-prefix value '$SUBNET_PREFIX'. Expected A.B.C"
        fi
        return 0
    fi

    for prefix in "${candidates[@]}"; do
        if ! grep -qE "^${prefix//./\\.}\.0/24" <<<"$routes"; then
            SUBNET_PREFIX="$prefix"
            return 0
        fi
    done

    die "Could not auto-pick a free /24 subnet. Provide --subnet-prefix."
}

pick_dns_servers() {
    local nameservers=()
    local line

    while read -r line; do
        if [[ "$line" =~ ^nameserver[[:space:]]+([0-9]{1,3}(\.[0-9]{1,3}){3})$ ]]; then
            if [[ "${BASH_REMATCH[1]}" != 127.* ]]; then
                nameservers+=("${BASH_REMATCH[1]}")
            fi
        fi
    done < /etc/resolv.conf

    if [[ -z "$HOTSPOT_DNS1" ]]; then
        HOTSPOT_DNS1="${nameservers[0]:-1.1.1.1}"
    fi
    if [[ -z "$HOTSPOT_DNS2" ]]; then
        HOTSPOT_DNS2="${nameservers[1]:-8.8.8.8}"
    fi
}

generate_defaults() {
    local host

    if [[ -z "$UPLINK_INTERFACE" ]]; then
        UPLINK_INTERFACE="$(default_uplink_interface)"
    fi
    if [[ -z "$UPLINK_INTERFACE" ]]; then
        die "Could not detect uplink interface from default route. Use --uplink IFACE."
    fi
    if ! ip link show "$UPLINK_INTERFACE" >/dev/null 2>&1; then
        die "Uplink interface '$UPLINK_INTERFACE' does not exist."
    fi

    pick_wireless_interface
    pick_ap_interface_name
    ensure_ap_support
    pick_subnet_prefix
    pick_dns_servers

    HOTSPOT_IP="${SUBNET_PREFIX}.1"
    HOTSPOT_DHCP_START="${SUBNET_PREFIX}.20"
    HOTSPOT_DHCP_END="${SUBNET_PREFIX}.250"

    host="$(hostname 2>/dev/null | tr -cd 'A-Za-z0-9-')"
    if [[ -z "$HOTSPOT_SSID" ]]; then
        HOTSPOT_SSID="LinuxHotspot-${host:-host}"
    fi
    if [[ -z "$HOTSPOT_PASSWORD" ]]; then
        HOTSPOT_PASSWORD="$(tr -dc 'A-Za-z0-9' </dev/urandom | head -c 12)"
    fi
    if [[ ${#HOTSPOT_PASSWORD} -lt 8 || ${#HOTSPOT_PASSWORD} -gt 63 ]]; then
        die "Hotspot password length must be between 8 and 63 characters."
    fi
    if ! [[ "$HOTSPOT_CHANNEL" =~ ^[0-9]+$ ]]; then
        die "Channel must be a number."
    fi
}

write_state() {
    mkdir -p "$STATE_DIR"
    {
        printf 'UPLINK_INTERFACE=%q\n' "$UPLINK_INTERFACE"
        printf 'WIFI_INTERFACE=%q\n' "$WIFI_INTERFACE"
        printf 'HOTSPOT_INTERFACE=%q\n' "$HOTSPOT_INTERFACE"
        printf 'HOTSPOT_IP=%q\n' "$HOTSPOT_IP"
        printf 'HOTSPOT_DHCP_START=%q\n' "$HOTSPOT_DHCP_START"
        printf 'HOTSPOT_DHCP_END=%q\n' "$HOTSPOT_DHCP_END"
        printf 'HOTSPOT_DNS1=%q\n' "$HOTSPOT_DNS1"
        printf 'HOTSPOT_DNS2=%q\n' "$HOTSPOT_DNS2"
        printf 'ORIGINAL_IP_FORWARD=%q\n' "$ORIGINAL_IP_FORWARD"
        printf 'DNSMASQ_PID_FILE=%q\n' "$DNSMASQ_PID_FILE"
        printf 'HOSTAPD_PID_FILE=%q\n' "$HOSTAPD_PID_FILE"
        printf 'RUNNING_DNSMASQ_PID=%q\n' "$RUNNING_DNSMASQ_PID"
        printf 'RUNNING_HOSTAPD_PID=%q\n' "$RUNNING_HOSTAPD_PID"
    } >"$STATE_FILE"
}

load_state() {
    if [[ ! -f "$STATE_FILE" ]]; then
        return 1
    fi
    # shellcheck disable=SC1090
    source "$STATE_FILE"
}

pid_is_alive() {
    local pid="$1"
    [[ -n "$pid" ]] && kill -0 "$pid" >/dev/null 2>&1
}

safe_kill() {
    local pid="$1"
    if pid_is_alive "$pid"; then
        kill "$pid" >/dev/null 2>&1 || true
        sleep 1
        if pid_is_alive "$pid"; then
            kill -9 "$pid" >/dev/null 2>&1 || true
        fi
    fi
}

cleanup_hotspot() {
    local pid

    load_state || true

    if [[ -f "${HOSTAPD_PID_FILE:-}" ]]; then
        pid="$(cat "$HOSTAPD_PID_FILE" 2>/dev/null || true)"
        safe_kill "$pid"
    else
        safe_kill "${RUNNING_HOSTAPD_PID:-}"
    fi

    if [[ -f "${DNSMASQ_PID_FILE:-}" ]]; then
        pid="$(cat "$DNSMASQ_PID_FILE" 2>/dev/null || true)"
        safe_kill "$pid"
    else
        safe_kill "${RUNNING_DNSMASQ_PID:-}"
    fi

    if [[ -n "${UPLINK_INTERFACE:-}" && -n "${HOTSPOT_INTERFACE:-}" ]]; then
        iptables -t nat -D POSTROUTING -o "$UPLINK_INTERFACE" -j MASQUERADE >/dev/null 2>&1 || true
        iptables -D FORWARD -i "$HOTSPOT_INTERFACE" -o "$UPLINK_INTERFACE" -j ACCEPT >/dev/null 2>&1 || true
        iptables -D FORWARD -i "$UPLINK_INTERFACE" -o "$HOTSPOT_INTERFACE" -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT >/dev/null 2>&1 || true
    fi

    if [[ -n "${ORIGINAL_IP_FORWARD:-}" ]]; then
        sysctl -w net.ipv4.ip_forward="$ORIGINAL_IP_FORWARD" >/dev/null 2>&1 || true
    fi

    if [[ -n "${HOTSPOT_INTERFACE:-}" ]] && ip link show "$HOTSPOT_INTERFACE" >/dev/null 2>&1; then
        ip link set "$HOTSPOT_INTERFACE" down >/dev/null 2>&1 || true
        iw dev "$HOTSPOT_INTERFACE" del >/dev/null 2>&1 || true
    fi

    rm -rf "$STATE_DIR"
}

trap_cleanup() {
    cleanup_hotspot
}

generate_dnsmasq_conf() {
    cat >"$DNSMASQ_CONF" <<EOF
interface=$HOTSPOT_INTERFACE
bind-interfaces
dhcp-range=$HOTSPOT_DHCP_START,$HOTSPOT_DHCP_END,$DHCP_LEASETIME
dhcp-option=3,$HOTSPOT_IP
dhcp-option=6,$HOTSPOT_DNS1,$HOTSPOT_DNS2
server=$HOTSPOT_DNS1
server=$HOTSPOT_DNS2
EOF
}

generate_hostapd_conf() {
    cat >"$HOSTAPD_CONF" <<EOF
interface=$HOTSPOT_INTERFACE
driver=nl80211
ssid=$HOTSPOT_SSID
hw_mode=g
channel=$HOTSPOT_CHANNEL
ieee80211n=1
wmm_enabled=1
auth_algs=1
wpa=2
wpa_key_mgmt=WPA-PSK
rsn_pairwise=CCMP
wpa_passphrase=$HOTSPOT_PASSWORD
EOF
}

create_ap_interface() {
    if [[ "$HOTSPOT_INTERFACE" == "$WIFI_INTERFACE" ]]; then
        die "AP interface must be different from wireless source interface."
    fi

    if ip link show "$HOTSPOT_INTERFACE" >/dev/null 2>&1; then
        warn "Interface '$HOTSPOT_INTERFACE' already exists. Re-creating it."
        ip link set "$HOTSPOT_INTERFACE" down >/dev/null 2>&1 || true
        iw dev "$HOTSPOT_INTERFACE" del >/dev/null 2>&1 || true
    fi

    if ! iw dev "$WIFI_INTERFACE" interface add "$HOTSPOT_INTERFACE" type __ap; then
        die "Failed to create AP interface on '$WIFI_INTERFACE'. Driver may not support AP+managed concurrency."
    fi

    ip addr flush dev "$HOTSPOT_INTERFACE" >/dev/null 2>&1 || true
    ip addr add "$HOTSPOT_IP/24" dev "$HOTSPOT_INTERFACE"
    ip link set "$HOTSPOT_INTERFACE" up
}

enable_forwarding_and_nat() {
    ORIGINAL_IP_FORWARD="$(sysctl -n net.ipv4.ip_forward 2>/dev/null || echo 1)"
    sysctl -w net.ipv4.ip_forward=1 >/dev/null

    iptables -t nat -C POSTROUTING -o "$UPLINK_INTERFACE" -j MASQUERADE >/dev/null 2>&1 || \
        iptables -t nat -A POSTROUTING -o "$UPLINK_INTERFACE" -j MASQUERADE

    iptables -C FORWARD -i "$HOTSPOT_INTERFACE" -o "$UPLINK_INTERFACE" -j ACCEPT >/dev/null 2>&1 || \
        iptables -A FORWARD -i "$HOTSPOT_INTERFACE" -o "$UPLINK_INTERFACE" -j ACCEPT

    iptables -C FORWARD -i "$UPLINK_INTERFACE" -o "$HOTSPOT_INTERFACE" -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT >/dev/null 2>&1 || \
        iptables -A FORWARD -i "$UPLINK_INTERFACE" -o "$HOTSPOT_INTERFACE" -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT
}

start_services() {
    dnsmasq --conf-file="$DNSMASQ_CONF" --pid-file="$DNSMASQ_PID_FILE" --keep-in-foreground &
    RUNNING_DNSMASQ_PID="$!"
    sleep 1
    if ! pid_is_alive "$RUNNING_DNSMASQ_PID"; then
        die "dnsmasq failed to start."
    fi

    hostapd -P "$HOSTAPD_PID_FILE" "$HOSTAPD_CONF" &
    RUNNING_HOSTAPD_PID="$!"
    sleep 1
    if ! pid_is_alive "$RUNNING_HOSTAPD_PID"; then
        die "hostapd failed to start."
    fi
}

run_hotspot() {
    require_root "$@"
    install_dependencies_if_needed
    generate_defaults

    if load_state && pid_is_alive "${RUNNING_HOSTAPD_PID:-}"; then
        die "Hotspot appears to already be running. Use '$SCRIPT_NAME stop' first."
    fi

    cleanup_hotspot >/dev/null 2>&1 || true
    mkdir -p "$STATE_DIR"
    trap trap_cleanup EXIT INT TERM

    log "Using uplink interface: $UPLINK_INTERFACE"
    log "Using wireless interface: $WIFI_INTERFACE"
    log "Creating AP interface: $HOTSPOT_INTERFACE"
    log "Hotspot subnet: $SUBNET_PREFIX.0/24"

    create_ap_interface
    generate_dnsmasq_conf
    generate_hostapd_conf
    enable_forwarding_and_nat
    start_services
    write_state

    log "Hotspot is active."
    log "SSID: $HOTSPOT_SSID"
    log "Password: $HOTSPOT_PASSWORD"
    log "Gateway: $HOTSPOT_IP"
    log "Press Ctrl+C to stop."

    while pid_is_alive "$RUNNING_HOSTAPD_PID" && pid_is_alive "$RUNNING_DNSMASQ_PID"; do
        sleep 1
    done

    die "A hotspot process exited unexpectedly."
}

stop_hotspot() {
    require_root "$@"
    if [[ ! -f "$STATE_FILE" ]]; then
        log "Hotspot is not running."
        return 0
    fi
    cleanup_hotspot
    log "Hotspot stopped."
}

status_hotspot() {
    if ! load_state; then
        echo "Hotspot status: inactive"
        return 0
    fi

    local hostapd_pid dnsmasq_pid
    hostapd_pid="$(cat "$HOSTAPD_PID_FILE" 2>/dev/null || true)"
    dnsmasq_pid="$(cat "$DNSMASQ_PID_FILE" 2>/dev/null || true)"

    if ! pid_is_alive "$hostapd_pid" || ! pid_is_alive "$dnsmasq_pid"; then
        echo "Hotspot status: stale (state exists but process is not running)"
        return 1
    fi

    echo "Hotspot status: active"
    echo "  uplink interface: $UPLINK_INTERFACE"
    echo "  wireless interface: $WIFI_INTERFACE"
    echo "  hotspot interface: $HOTSPOT_INTERFACE"
    echo "  gateway: $HOTSPOT_IP"
    echo "  hostapd pid: ${hostapd_pid:-unknown}"
    echo "  dnsmasq pid: ${dnsmasq_pid:-unknown}"
}

main() {
    parse_args "$@"

    case "$ACTION" in
        run)
            run_hotspot "$@"
            ;;
        stop)
            stop_hotspot "$@"
            ;;
        status)
            status_hotspot
            ;;
        *)
            die "Unknown action '$ACTION'"
            ;;
    esac
}

main "$@"
