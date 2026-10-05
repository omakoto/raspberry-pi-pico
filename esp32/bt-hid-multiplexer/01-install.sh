#!/bin/bash
#
# Flashes the ESP32-S3 BLE HID Multiplexer firmware.
#
# Port selection, in this order:
#   1. A port given with -p (or $ESPPORT).
#   2. DevKitC builds only, unless -u is given: the on-board USB-UART bridge (CP210x / CH34x), which esptool resets into
#      download mode by itself through DTR/RTS.
#   3. A ROM (or USB-Serial-JTAG console) download port on the native USB port (303a:1001).
#   4. Otherwise the running firmware is asked to reboot into ROM download mode, through the VIA
#      "jump to bootloader" command on the VIAL raw HID interface (always present) or the "bootloader"
#      command on its own USB serial port (when enabled), and step 3 is retried.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

usage() {
    cat <<'EOF'
Usage: 01-install.sh [options] [-- extra idf.py arguments]

Flashes build/ to the board (building first if there is no build yet).

Options:
  -b, --board BOARD   Refuse to flash unless the build is for this board (devkitc or xiao).
                      Without it, the build is flashed whatever board it is for.
  -p, --port PORT     Serial port to flash through (default: auto-detected, see below).
  -u, --usb           Flash through the native USB port even when the DevKitC's UART bridge
                      is connected (the way a XIAO is always flashed).
  -h, --help          Shows this help.

Port auto-detection:
  - DevKitC builds: the on-board USB-UART bridge port, if connected.
  - The native USB port in ROM download mode (/dev/serial/by-id/*Espressif*USB_JTAG*).
  - Otherwise the running firmware is asked to reboot into download mode over USB.
  If nothing works: hold BOOT, tap RESET, release BOOT, and run this again.

Examples:
  ./01-install.sh
  ./01-install.sh -b xiao
  ./01-install.sh -p /dev/ttyUSB0
  ./01-install.sh -u
EOF
}

OPTS=$(getopt -o b:p:uh --long board:,port:,usb,help -n "$(basename "$0")" -- "$@") || { usage >&2; exit 1; }
eval set -- "$OPTS"

WANT_BOARD=""
PORT="${ESPPORT:-}"
USB_ONLY=0
while true; do
    case "$1" in
        -b|--board) WANT_BOARD="$2"; shift 2 ;;
        -p|--port) PORT="$2"; shift 2 ;;
        -u|--usb) USB_ONLY=1; shift ;;
        -h|--help) usage; exit 0 ;;
        --) shift; break ;;
        *) echo "Internal error parsing options" >&2; exit 1 ;;
    esac
done

cd "$SCRIPT_DIR"

if [[ ! -f "build/bt-hid-multiplexer.bin" ]]; then
    echo "Firmware binary not found. Running 00-build.sh first..."
    if [[ -n "$WANT_BOARD" ]]; then
        ./00-build.sh -b "$WANT_BOARD"
    else
        ./00-build.sh
    fi
fi

BUILT_BOARD="$(grep -E '^BOARD:' build/CMakeCache.txt | cut -d= -f2 || true)"
if [[ -n "$WANT_BOARD" && "$WANT_BOARD" != "$BUILT_BOARD" ]]; then
    echo "Error: build/ is for board '${BUILT_BOARD}', not '${WANT_BOARD}'. Run ./00-build.sh -b ${WANT_BOARD}." >&2
    exit 1
fi
echo "Flashing the build for board '${BUILT_BOARD}'."

find_rom_port() {
    local p
    for p in /dev/serial/by-id/*Espressif*USB_JTAG*; do
        if [[ -e "$p" ]]; then
            echo "$p"
            return 0
        fi
    done
    return 1
}

find_uart_bridge() {
    local p
    for p in /dev/serial/by-id/*; do
        case "$p" in
            *CP210*|*Silicon_Labs*|*1a86*|*CH34*|*CH9102*) echo "$p"; return 0 ;;
        esac
    done
    return 1
}

# Asks the running firmware to reboot into ROM download mode.
request_download_mode() {
    # VIA "jump to bootloader" (0x0B) on the VIAL raw HID interface. Needs access to the hidraw node
    # (see ~/cbin/setup/config-hidraw-permission). The PID is shared with other TinyUSB gadgets, so the
    # interface is also identified by its VIAL usage page.
    python3 - <<'PYEOF' 2>/dev/null || true
import glob, os
for d in sorted(glob.glob('/sys/class/hidraw/hidraw*')):
    try:
        if '0003:0000303A:00004004' not in open(d + '/device/uevent').read():
            continue
        desc = open(d + '/device/report_descriptor', 'rb').read()
        if not desc.startswith(bytes([0x06, 0x60, 0xFF])):  # vendor usage page 0xFF60 (VIAL)
            continue
        fd = os.open('/dev/' + os.path.basename(d), os.O_WRONLY)
        os.write(fd, bytes([0, 0x0B]) + bytes(31))
        os.close(fd)
        print('Sent bootloader command over VIAL RawHID /dev/' + os.path.basename(d))
        break
    except Exception:
        pass
PYEOF

    # The firmware's own USB serial port (only present when enabled at boot).
    local cdc_dev
    for cdc_dev in /dev/serial/by-id/usb-omakoto_ESP32-S3_BLE_HID_Multiplexer*-if00; do
        if [[ -e "$cdc_dev" ]]; then
            echo "Sending 'bootloader' to the USB serial console $cdc_dev..."
            python3 -c "
import os
try:
    fd = os.open('$cdc_dev', os.O_RDWR | os.O_NOCTTY)
    os.write(fd, b'bootloader\r\n')
    os.close(fd)
except Exception:
    pass
" 2>/dev/null || true
        fi
    done
}

if [[ -z "$PORT" && "$BUILT_BOARD" == "devkitc" && $USB_ONLY -eq 0 ]]; then
    PORT="$(find_uart_bridge || true)"
fi
if [[ -z "$PORT" ]]; then
    PORT="$(find_rom_port || true)"
fi
if [[ -z "$PORT" ]]; then
    request_download_mode
    echo "Waiting for the board in download mode..."
    for _ in {1..12}; do
        PORT="$(find_rom_port || true)"
        [[ -n "$PORT" ]] && break
        sleep 0.5
    done
fi
if [[ -z "$PORT" ]]; then
    echo "Error: no flashing port found." >&2
    echo "Hold BOOT, tap RESET, release BOOT, then run this script again." >&2
    exit 1
fi

# Activate ESP-IDF environment
IDF_PATH="${IDF_PATH:-$HOME/esp-idf}"
if [[ -f "${IDF_PATH}/export.sh" ]]; then
    unset IDF_PYTHON_ENV_PATH
    # shellcheck source=/dev/null
    . "${IDF_PATH}/export.sh" >/dev/null 2>&1
else
    echo "Error: ESP-IDF not found at '${IDF_PATH}'. Please set IDF_PATH or install ESP-IDF." >&2
    exit 1
fi

echo "Flashing through ${PORT}..."
idf.py -p "$PORT" flash "$@"
