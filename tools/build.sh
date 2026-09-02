#!/usr/bin/env bash
#
# build.sh — Build & flash ESP BLE Device firmware
#
# Usage:
#   ./tools/build.sh [device] [command] [idf_version] [port]
#
# Examples:
#   ./tools/build.sh                          # interactive: pick device + command
#   ./tools/build.sh reference_device         # build reference_device
#   ./tools/build.sh reference_device flash   # build + flash
#   ./tools/build.sh reference_device flash /dev/tty.usbmodem*  # custom port
#   ./tools/build.sh reference_device monitor # build + flash + monitor
#   ./tools/build.sh reference_device clean    # full clean + build
#   IDF_VERSION=6.1-rc1 ./tools/build.sh reference_device flash
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DEVICES_DIR="$PROJECT_ROOT/devices"

# ── Colors ───────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m' # No Color

info()  { echo -e "${CYAN}[INFO]${NC}  $*"; }
ok()    { echo -e "${GREEN}[OK]${NC}    $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
fail()  { echo -e "${RED}[FAIL]${NC}  $*"; exit 1; }

# ── Discover available devices ───────────────────────────────────
discover_devices() {
    local devices=()
    for d in "$DEVICES_DIR"/*/; do
        [ -d "$d" ] && devices+=("$(basename "$d")")
    done
    echo "${devices[@]}"
}

# ── Select device interactively ──────────────────────────────────
select_device() {
    local devices
    read -ra devices <<< "$(discover_devices)"

    if [ ${#devices[@]} -eq 0 ]; then
        fail "No devices found in $DEVICES_DIR"
    fi

    if [ ${#devices[@]} -eq 1 ]; then
        echo "${devices[0]}"
        return
    fi

    echo -e "${BOLD}Available devices:${NC}"
    for i in "${!devices[@]}"; do
        echo -e "  ${GREEN}$((i+1))${NC}) ${devices[$i]}"
    done
    echo ""
    read -rp "Select device [1-${#devices[@]}]: " choice

    if ! [[ "$choice" =~ ^[0-9]+$ ]] || [ "$choice" -lt 1 ] || [ "$choice" -gt ${#devices[@]} ]; then
        fail "Invalid selection: $choice"
    fi
    echo "${devices[$((choice-1))]}"
}

# ── Select command interactively ─────────────────────────────────
select_command() {
    local hint="${1:-}"
    local commands=("build" "flash" "monitor" "clean")

    if [ -n "$hint" ]; then
        echo "$hint"
        return
    fi

    echo -e "${BOLD}Commands:${NC}"
    echo -e "  ${GREEN}1${NC}) build       — compile only"
    echo -e "  ${GREEN}2${NC}) flash       — compile + flash"
    echo -e "  ${GREEN}3${NC}) monitor     — compile + flash + serial monitor"
    echo -e "  ${GREEN}4${NC}) clean       — full clean + compile"
    echo ""
    read -rp "Select command [1-4]: " choice

    case "$choice" in
        1) echo "build" ;;
        2) echo "flash" ;;
        3) echo "monitor" ;;
        4) echo "clean" ;;
        *) fail "Invalid selection: $choice" ;;
    esac
}

# ── Resolve IDF version ──────────────────────────────────────────
resolve_idf_path() {
    local version="${IDF_VERSION:-v5.5.5}"
    local idf_path="$HOME/.espressif/$version/esp-idf"

    # Try without 'v' prefix for user-friendly input (e.g. "5.5.5")
    if [ ! -d "$idf_path" ] && [[ ! "$version" =~ ^v ]]; then
        idf_path="$HOME/.espressif/v$version/esp-idf"
    fi

    if [ ! -d "$idf_path" ]; then
        # Try listing available versions
        local available
        available=$(ls "$HOME/.espressif/" 2>/dev/null | grep -E '^v[0-9]' | tr '\n' ' ')
        fail "ESP-IDF version '$version' not found at $idf_path\n    Available: ${available:-none}"
    fi

    echo "$idf_path"
}

# ── Detect serial port ───────────────────────────────────────────
detect_port() {
    local explicit_port="${1:-}"
    if [ -n "$explicit_port" ] && [ "$explicit_port" != "-" ]; then
        echo "$explicit_port"
        return
    fi

    # Auto-detect on macOS and Linux
    local ports=()
    if [[ "$OSTYPE" == "darwin"* ]]; then
        # macOS: look for common ESP32 USB serial devices
        mapfile -t ports < <(ls /dev/cu.usbmodem* /dev/cu.usbserial* 2>/dev/null || true)
    else
        mapfile -t ports < <(ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null || true)
    fi

    if [ ${#ports[@]} -eq 0 ]; then
        warn "No serial port detected. Flash may fail."
        echo ""
        return
    fi

    if [ ${#ports[@]} -eq 1 ]; then
        info "Auto-detected port: ${ports[0]}"
        echo "${ports[0]}"
        return
    fi

    echo -e "${BOLD}Serial ports:${NC}"
    for i in "${!ports[@]}"; do
        echo -e "  ${GREEN}$((i+1))${NC}) ${ports[$i]}"
    done
    echo -e "  ${GREEN}0${NC}) skip (no port)"
    echo ""
    read -rp "Select port [0-${#ports[@]}]: " choice

    if [ "$choice" = "0" ] || [ -z "$choice" ]; then
        echo ""
        return
    fi
    echo "${ports[$((choice-1))]}"
}

# ── Main ─────────────────────────────────────────────────────────
main() {
    local device="${1:-}"
    local command="${2:-}"
    local port="${3:-}"

    # Interactive prompts
    if [ -z "$device" ]; then
        device=$(select_device)
    fi

    if [ ! -d "$DEVICES_DIR/$device" ]; then
        fail "Device '$device' not found. Available: $(discover_devices)"
    fi

    command=$(select_command "$command")
    port=$(detect_port "$port")

    local device_dir="$DEVICES_DIR/$device"
    local idf_path
    idf_path=$(resolve_idf_path)

    echo ""
    echo -e "${BOLD}══════════════════════════════════════════${NC}"
    echo -e "${BOLD}  Device:   ${GREEN}$device${NC}"
    echo -e "${BOLD}  Command:  ${GREEN}$command${NC}"
    echo -e "${BOLD}  IDF:      ${CYAN}$idf_path${NC}"
    [ -n "$port" ] && echo -e "${BOLD}  Port:     ${CYAN}$port${NC}"
    echo -e "${BOLD}══════════════════════════════════════════${NC}"
    echo ""

    # Source ESP-IDF environment
    info "Sourcing ESP-IDF environment..."
    # shellcheck disable=SC1091
    source "$idf_path/export.sh" > /dev/null 2>&1
    ok "IDF_PATH=$IDF_PATH"

    # Enter device directory
    cd "$device_dir"

    # Execute command
    case "$command" in
        clean)
            info "Full clean..."
            idf.py fullclean
            info "Building..."
            idf.py build
            ok "Build complete"
            if [ -n "$port" ]; then
                info "Flashing to $port..."
                idf.py -p "$port" flash
                ok "Flash complete"
                info "Starting monitor..."
                idf.py -p "$port" monitor
            else
                warn "No port — skipping flash"
            fi
            ;;
        build)
            info "Building..."
            idf.py build
            ok "Build complete"
            ;;
        flash)
            info "Building..."
            idf.py build
            ok "Build complete"
            if [ -n "$port" ]; then
                info "Flashing to $port..."
                idf.py -p "$port" flash
                ok "Flash complete"
            else
                warn "No port — skipping flash"
            fi
            ;;
        monitor)
            info "Building..."
            idf.py build
            ok "Build complete"
            if [ -n "$port" ]; then
                info "Flashing to $port..."
                idf.py -p "$port" flash
                ok "Flash complete"
                info "Starting monitor (Ctrl+] to exit)..."
                idf.py -p "$port" monitor
            else
                warn "No port — skipping flash/monitor"
            fi
            ;;
        *)
            fail "Unknown command: $command"
            ;;
    esac
}

main "$@"
