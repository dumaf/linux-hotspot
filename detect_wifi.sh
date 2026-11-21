#!/usr/bin/env bash
# detect_wifi.sh - Identify Wi-Fi hardware, driver, AP mode capabilities, and active channel
# Usage:
#   ./detect_wifi.sh              # Human-readable summary
#   ./detect_wifi.sh --get-iface  # Outputs only the selected interface
#   ./detect_wifi.sh --export     # Outputs bash eval-ready environment variables
#   source ./detect_wifi.sh       # Sourced directly from hotspot script

set -Eeuo pipefail

# ── Color Definitions ────────────────────────────────────────────────────────
RED='\033[1;31m'
GREEN='\033[1;32m'
YELLOW='\033[1;33m'
BLUE='\033[1;34m'
CYAN='\033[1;36m'
BOLD='\033[1m'
NC='\033[0m'

# ── Global Output Variables ──────────────────────────────────────────────────
WIFI_IFACE="${WIFI_IFACE:-}"
WIFI_PHY=""
WIFI_DRIVER=""
WIFI_MODEL=""
WIFI_MAC=""
AP_SUPPORTED=0
CONCURRENCY_SUPPORTED=0
STA_CHANNEL=""
STA_FREQ=""
STA_SSID=""

# ── Helper Functions ─────────────────────────────────────────────────────────
has_cmd() { command -v "$1" >/dev/null 2>&1; }

# Find all wireless interfaces available on the system
list_wireless_interfaces() {
    local ifaces=()
    local iface

    # Method 1: Check /sys/class/net/*/wireless
    for iface in /sys/class/net/*/wireless; do
        if [[ -e "$iface" ]]; then
            iface="${iface%/wireless}"
            iface="${iface##*/}"
            [[ -n "$iface" && "$iface" != "*" ]] && ifaces+=("$iface")
        fi
    done

    # Method 2: Check /sys/class/net/*/phy80211
    for iface in /sys/class/net/*/phy80211; do
        if [[ -e "$iface" ]]; then
            iface="${iface%/phy80211}"
            iface="${iface##*/}"
            if [[ -n "$iface" && "$iface" != "*" ]]; then
                # Avoid duplicates
                if [[ ! " ${ifaces[*]:-} " =~ [[:space:]]${iface}[[:space:]] ]]; then
                    ifaces+=("$iface")
                fi
            fi
        fi
    done

    # Method 3: Fallback to iw dev
    if [[ ${#ifaces[@]} -eq 0 ]] && has_cmd iw; then
        while read -r line; do
            if [[ "$line" =~ Interface[[:space:]]+([^[:space:]]+) ]]; then
                local dev="${BASH_REMATCH[1]}"
                if [[ ! " ${ifaces[*]:-} " =~ [[:space:]]${dev}[[:space:]] ]]; then
                    ifaces+=("$dev")
                fi
            fi
        done < <(iw dev 2>/dev/null || true)
    fi

    echo "${ifaces[@]:-}"
}

# Resolve physical device name (e.g. phy0) for a given interface
get_phy_for_iface() {
    local iface="$1"
    local phy=""

    if [[ -d "/sys/class/net/$iface/phy80211" ]]; then
        phy="$(basename "$(readlink -f "/sys/class/net/$iface/phy80211" 2>/dev/null || true)" 2>/dev/null || true)"
    fi

    if [[ -z "$phy" ]] && has_cmd iw; then
        local wiphy_num
        wiphy_num="$(iw dev "$iface" info 2>/dev/null | grep -oP 'wiphy \K[0-9]+' || true)"
        [[ -n "$wiphy_num" ]] && phy="phy${wiphy_num}"
    fi

    echo "$phy"
}

# Detect kernel driver for a given interface
get_driver_for_iface() {
    local iface="$1"
    local driver=""

    if [[ -L "/sys/class/net/$iface/device/driver" ]]; then
        driver="$(basename "$(readlink -f "/sys/class/net/$iface/device/driver" 2>/dev/null || true)" 2>/dev/null || true)"
    fi

    if [[ -z "$driver" ]] && has_cmd ethtool; then
        driver="$(ethtool -i "$iface" 2>/dev/null | awk -F': ' '$1=="driver"{print $2}' || true)"
    fi

    echo "${driver:-unknown}"
}

# Detect hardware model / vendor description via PCI/USB
get_model_for_iface() {
    local iface="$1"
    local model=""

    # Try PCI device lookup
    if [[ -d "/sys/class/net/$iface/device" ]] && has_cmd lspci; then
        local dev_path
        dev_path="$(readlink -f "/sys/class/net/$iface/device" 2>/dev/null || true)"
        local pci_slot
        pci_slot="$(basename "$dev_path" 2>/dev/null || true)"
        if [[ "$pci_slot" =~ ^[0-9a-fA-F]{4}: ]]; then
            # Full domain:bus:dev.func format
            model="$(lspci -s "$pci_slot" 2>/dev/null | sed -E 's/^[^:]+: //' || true)"
        elif [[ "$pci_slot" =~ ^[0-9a-fA-F]{2}: ]]; then
            model="$(lspci -s "$pci_slot" 2>/dev/null | sed -E 's/^[^:]+: //' || true)"
        fi
    fi

    # Try USB device lookup
    if [[ -z "$model" && -d "/sys/class/net/$iface/device" ]]; then
        local usb_product=""
        local usb_mfr=""
        if [[ -f "/sys/class/net/$iface/device/../product" ]]; then
            usb_product="$(cat "/sys/class/net/$iface/device/../product" 2>/dev/null || true)"
        fi
        if [[ -f "/sys/class/net/$iface/device/../manufacturer" ]]; then
            usb_mfr="$(cat "/sys/class/net/$iface/device/../manufacturer" 2>/dev/null || true)"
        fi
        if [[ -n "$usb_product" || -n "$usb_mfr" ]]; then
            model="${usb_mfr:+${usb_mfr} }${usb_product:-USB Wireless Device}"
        fi
    fi

    # Fallback to driver info
    if [[ -z "$model" ]]; then
        local driver
        driver="$(get_driver_for_iface "$iface")"
        model="Generic Wi-Fi Adapter (driver: ${driver})"
    fi

    echo "$model"
}

# Check if AP mode is supported on a given physical device
check_phy_ap_support() {
    local phy="$1"
    if [[ -z "$phy" ]] || ! has_cmd iw; then
        echo 0
        return
    fi

    local phy_info
    phy_info="$(iw phy "$phy" info 2>/dev/null || true)"
    if echo "$phy_info" | grep -qP '^\s+\* AP$'; then
        echo 1
    else
        echo 0
    fi
}

# Check if concurrent AP + STA mode is supported
check_phy_concurrency() {
    local phy="$1"
    if [[ -z "$phy" ]] || ! has_cmd iw; then
        echo 0
        return
    fi

    local phy_info
    phy_info="$(iw phy "$phy" info 2>/dev/null || true)"
    # Check if 'valid interface combinations' section has AP and managed
    local combo_section
    combo_section="$(echo "$phy_info" | awk '/valid interface combinations:/,/^(\t|\s)*$/')"
    if echo "$combo_section" | grep -q "AP" && echo "$combo_section" | grep -q "managed"; then
        echo 1
    else
        echo 0
    fi
}

# Query current connection details for an interface (SSID, channel, frequency)
get_connection_info() {
    local iface="$1"
    local channel=""
    local freq=""
    local ssid=""

    if has_cmd iw; then
        local dev_info
        dev_info="$(iw dev "$iface" info 2>/dev/null || true)"
        channel="$(echo "$dev_info" | grep -oP 'channel \K[0-9]+' | head -1 || true)"
        freq="$(echo "$dev_info" | grep -oP 'channel [0-9]+ \(\K[0-9]+' | head -1 || true)"

        local link_info
        link_info="$(iw dev "$iface" link 2>/dev/null || true)"
        if [[ "$link_info" =~ SSID:[[:space:]]+(.*) ]]; then
            ssid="${BASH_REMATCH[1]}"
        fi
        if [[ -z "$freq" && "$link_info" =~ freq:[[:space:]]+([0-9]+) ]]; then
            freq="${BASH_REMATCH[1]}"
        fi
    fi

    if [[ -z "$channel" ]] && has_cmd iwconfig; then
        local iwc
        iwc="$(iwconfig "$iface" 2>/dev/null || true)"
        channel="$(echo "$iwc" | grep -oP 'Channel[=:]\K[0-9]+' | head -1 || true)"
        freq="$(echo "$iwc" | grep -oP 'Frequency[=:]\K[0-9.]+' | head -1 || true)"
        if [[ "$iwc" =~ ESSID:\"([^\"]+)\" ]]; then
            ssid="${BASH_REMATCH[1]}"
        fi
    fi

    echo "${channel}|${freq}|${ssid}"
}

# ── Main Card Evaluation & Selection ─────────────────────────────────────────
identify_wifi_card() {
    local preferred_iface="${1:-$WIFI_IFACE}"
    local detected_ifaces=()
    read -r -a detected_ifaces <<< "$(list_wireless_interfaces)"

    if [[ ${#detected_ifaces[@]} -eq 0 ]]; then
        # Check if rfkill is blocking
        if has_cmd rfkill && rfkill list wifi 2>/dev/null | grep -qi "yes"; then
            echo -e "${RED}[wifi-detect] ERROR: No active Wi-Fi interfaces found, and rfkill reports Wi-Fi is BLOCKED.${NC}" >&2
            echo -e "${YELLOW}[wifi-detect] Try unblocking: sudo rfkill unblock wifi${NC}" >&2
        else
            echo -e "${RED}[wifi-detect] ERROR: No wireless network interfaces detected.${NC}" >&2
        fi
        return 1
    fi

    # Check preferred interface if provided
    if [[ -n "$preferred_iface" ]]; then
        local found=0
        for iface in "${detected_ifaces[@]}"; do
            if [[ "$iface" == "$preferred_iface" ]]; then
                found=1
                break
            fi
        done
        if [[ $found -eq 0 ]]; then
            echo -e "${RED}[wifi-detect] ERROR: Configured interface '$preferred_iface' does not exist.${NC}" >&2
            return 1
        fi
    fi

    # Inspect all interfaces and pick the best one
    local best_iface=""
    local best_score=-1

    # Arrays to store device properties
    declare -A card_phy card_driver card_model card_mac card_ap card_conc card_chan card_freq card_ssid

    for iface in "${detected_ifaces[@]}"; do
        local phy
        phy="$(get_phy_for_iface "$iface")"
        local driver
        driver="$(get_driver_for_iface "$iface")"
        local model
        model="$(get_model_for_iface "$iface")"
        local mac
        mac="$(cat "/sys/class/net/$iface/address" 2>/dev/null || ip link show dev "$iface" 2>/dev/null | awk '/link\/ether/{print $2}' || echo "unknown")"
        local ap
        ap="$(check_phy_ap_support "$phy")"
        local conc
        conc="$(check_phy_concurrency "$phy")"

        local conn_info
        conn_info="$(get_connection_info "$iface")"
        local chan="${conn_info%%|*}"
        local rem="${conn_info#*|}"
        local freq="${rem%%|*}"
        local ssid="${rem#*|}"

        card_phy["$iface"]="$phy"
        card_driver["$iface"]="$driver"
        card_model["$iface"]="$model"
        card_mac["$iface"]="$mac"
        card_ap["$iface"]="$ap"
        card_conc["$iface"]="$conc"
        card_chan["$iface"]="$chan"
        card_freq["$iface"]="$freq"
        card_ssid["$iface"]="$ssid"

        # Calculate score for auto-selection
        local score=0
        [[ "$ap" -eq 1 ]] && score=$((score + 100))
        [[ "$conc" -eq 1 ]] && score=$((score + 50))
        [[ -n "$ssid" ]] && score=$((score + 30))
        [[ -n "$chan" ]] && score=$((score + 10))

        # If user explicitly preferred this interface, give huge boost
        if [[ -n "$preferred_iface" && "$iface" == "$preferred_iface" ]]; then
            score=$((score + 1000))
        fi

        if [[ $score -gt $best_score ]]; then
            best_score=$score
            best_iface="$iface"
        fi
    done

    # Set globals for selected card
    WIFI_IFACE="$best_iface"
    WIFI_PHY="${card_phy[$best_iface]:-}"
    WIFI_DRIVER="${card_driver[$best_iface]:-}"
    WIFI_MODEL="${card_model[$best_iface]:-}"
    WIFI_MAC="${card_mac[$best_iface]:-}"
    AP_SUPPORTED="${card_ap[$best_iface]:-0}"
    CONCURRENCY_SUPPORTED="${card_conc[$best_iface]:-0}"
    STA_CHANNEL="${card_chan[$best_iface]:-}"
    STA_FREQ="${card_freq[$best_iface]:-}"
    STA_SSID="${card_ssid[$best_iface]:-}"

    # Fallback channel if not connected
    if [[ -z "$STA_CHANNEL" ]]; then
        STA_CHANNEL="6"
        STA_FREQ="2437"
    fi

    return 0
}

# ── Formatted Output Report ──────────────────────────────────────────────────
print_report() {
    echo ""
    echo -e "${CYAN}╔════════════════════════════════════════════════════════════════╗${NC}"
    echo -e "${CYAN}║${NC}             ${BOLD}Wi-Fi Hardware Identification Report${NC}               ${CYAN}║${NC}"
    echo -e "${CYAN}╚════════════════════════════════════════════════════════════════╝${NC}"
    echo ""

    local detected_ifaces=()
    read -r -a detected_ifaces <<< "$(list_wireless_interfaces)"

    local i=1
    for iface in "${detected_ifaces[@]}"; do
        local is_selected=0
        [[ "$iface" == "$WIFI_IFACE" ]] && is_selected=1

        local marker=" "
        local prefix=""
        if [[ $is_selected -eq 1 ]]; then
            marker="►"
            prefix=" ${GREEN}[SELECTED FOR HOTSPOT]${NC}"
        fi

        echo -e "${BOLD}${marker} Card #$i: ${CYAN}${iface}${NC}${prefix}"
        local phy
        phy="$(get_phy_for_iface "$iface")"
        local model
        model="$(get_model_for_iface "$iface")"
        local driver
        driver="$(get_driver_for_iface "$iface")"
        local mac
        mac="$(cat "/sys/class/net/$iface/address" 2>/dev/null || echo "unknown")"
        local ap
        ap="$(check_phy_ap_support "$phy")"
        local conc
        conc="$(check_phy_concurrency "$phy")"
        local conn_info
        conn_info="$(get_connection_info "$iface")"
        local chan="${conn_info%%|*}"
        local rem="${conn_info#*|}"
        local freq="${rem%%|*}"
        local ssid="${rem#*|}"

        echo -e "    ${BOLD}Physical Device:${NC}  ${phy:-unknown}"
        echo -e "    ${BOLD}Hardware Model:${NC}   $model"
        echo -e "    ${BOLD}Kernel Driver:${NC}    $driver"
        echo -e "    ${BOLD}MAC Address:${NC}      $mac"

        if [[ "$ap" -eq 1 ]]; then
            echo -e "    ${BOLD}AP Mode:${NC}          ${GREEN}Supported [YES]${NC}"
        else
            echo -e "    ${BOLD}AP Mode:${NC}          ${RED}Not Supported [NO]${NC}"
        fi

        if [[ "$conc" -eq 1 ]]; then
            echo -e "    ${BOLD}Concurrency:${NC}      ${GREEN}AP + Station Simultaneous [YES]${NC}"
        else
            echo -e "    ${BOLD}Concurrency:${NC}      ${YELLOW}Single Mode Only [NO/Unspecified]${NC}"
        fi

        if [[ -n "$ssid" ]]; then
            echo -e "    ${BOLD}Connection:${NC}       ${GREEN}Connected to '${ssid}'${NC} (Channel ${chan:-unknown}, ${freq:-unknown} MHz)"
        else
            echo -e "    ${BOLD}Connection:${NC}       ${YELLOW}Not connected to upstream Wi-Fi${NC} (defaults to Channel 6)"
        fi
        echo ""
        i=$((i + 1))
    done

    echo -e "${CYAN}────────────────────────────────────────────────────────────────${NC}"
    echo -e "  ${BOLD}Hotspot will use:${NC} ${GREEN}${WIFI_IFACE}${NC} (${WIFI_MODEL})"
    echo -e "  ${BOLD}Operating Channel:${NC} ${STA_CHANNEL} (${STA_FREQ:-2437} MHz)"
    echo -e "${CYAN}────────────────────────────────────────────────────────────────${NC}"
    echo ""
}

# ── CLI Options / Direct Invocation ──────────────────────────────────────────
main() {
    local mode="report"
    local target_iface=""

    while [[ $# -gt 0 ]]; do
        case "$1" in
            --get-iface|--iface-only)
                mode="iface_only"
                shift
                ;;
            --export)
                mode="export"
                shift
                ;;
            --iface)
                target_iface="${2:-}"
                shift 2
                ;;
            -h|--help)
                echo "Usage: $0 [OPTIONS]"
                echo ""
                echo "Options:"
                echo "  --get-iface   Output only the selected Wi-Fi interface name"
                echo "  --export      Output bash export commands for environment variables"
                echo "  --iface NAME  Specify and validate a specific Wi-Fi interface"
                echo "  -h, --help    Show this help message"
                exit 0
                ;;
            *)
                shift
                ;;
        esac
    done

    identify_wifi_card "$target_iface" || exit 1

    case "$mode" in
        iface_only)
            echo "$WIFI_IFACE"
            ;;
        export)
            cat <<EOF
export WIFI_IFACE="${WIFI_IFACE}"
export WIFI_PHY="${WIFI_PHY}"
export WIFI_DRIVER="${WIFI_DRIVER}"
export WIFI_MODEL="${WIFI_MODEL}"
export WIFI_MAC="${WIFI_MAC}"
export AP_SUPPORTED="${AP_SUPPORTED}"
export CONCURRENCY_SUPPORTED="${CONCURRENCY_SUPPORTED}"
export STA_CHANNEL="${STA_CHANNEL}"
export STA_FREQ="${STA_FREQ}"
EOF
            ;;
        report)
            print_report
            ;;
    esac
}

# If script is run directly (not sourced), execute main
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    main "$@"
fi
