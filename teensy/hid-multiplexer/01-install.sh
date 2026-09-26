#!/bin/bash
#
# Flashes the compiled HID multiplexer firmware to a Teensy 4.1 board using PlatformIO (teensy-cli).
# Supports triggering bootloader mode via Hardware UART (Pins 0/1) or USB CDC serial.
#
# Usage:
#   ./01-install.sh [--uart <port>] [pio-upload-options]
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export PATH="$HOME/.local/bin:$HOME/p3/bin:$PATH"

BOARD="teensy41"
UART_PORT="${TEENSY_UART:-}"
EXTRA_ARGS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --uart)
            if [[ $# -lt 2 ]]; then
                echo "Error: --uart requires a device path argument" >&2
                exit 1
            fi
            UART_PORT="$2"
            shift 2
            ;;
        -h|--help)
            echo "Usage: $0 [--uart <port>] [pio-upload-options]"
            exit 0
            ;;
        *)
            EXTRA_ARGS+=("$1")
            shift
            ;;
    esac
done

cd "$SCRIPT_DIR"

FIRMWARE_HEX=".pio/build/${BOARD}/firmware.hex"

if [[ ! -f "$FIRMWARE_HEX" ]]; then
    echo "Build artifact ($FIRMWARE_HEX) not found. Running 00-build.sh first..."
    ./00-build.sh
fi

if ! command -v pio >/dev/null 2>&1; then
    echo "Error: 'pio' (PlatformIO Core) not found in PATH." >&2
    echo "Install it via /home/omakoto/cbin/setup/install-teensy" >&2
    exit 1
fi

# Function to check if Teensy HalfKay bootloader is visible in lsusb
is_bootloader_present() {
    if command -v lsusb >/dev/null 2>&1; then
        lsusb -d 16c0:0478 >/dev/null 2>&1
        return $?
    fi
    return 1
}

# Auto-detect UART port if not specified
if [[ -z "$UART_PORT" ]]; then
    for candidate in /dev/ttyUSB0 /dev/ttyUSB1 /dev/ttyACM0 /dev/ttyACM1; do
        if [[ -e "$candidate" ]]; then
            UART_PORT="$candidate"
            break
        fi
    done
fi

# If bootloader is not currently detected, try triggering via UART
if ! is_bootloader_present; then
    if [[ -n "$UART_PORT" && -e "$UART_PORT" ]]; then
        echo "Teensy bootloader (16c0:0478) not found. Sending 'bootloader' command to ${UART_PORT} (115200 baud)..."
        stty -F "$UART_PORT" 115200 cs8 -cstopb -parenb -echo raw 2>/dev/null || true
        printf "\nbootloader\n" > "$UART_PORT" 2>/dev/null || true
        
        # Wait up to 3 seconds for bootloader to appear
        for _ in {1..6}; do
            sleep 0.5
            if is_bootloader_present; then
                echo "Teensy HalfKay bootloader detected! Allowing USB to settle..."
                sleep 1.5
                break
            fi
        done
    fi
fi

TEENSY_LOADER_CLI=""
if command -v teensy_loader_cli >/dev/null 2>&1; then
    TEENSY_LOADER_CLI="$(command -v teensy_loader_cli)"
elif [[ -x "$HOME/.platformio/packages/tool-teensy/teensy_loader_cli" ]]; then
    TEENSY_LOADER_CLI="$HOME/.platformio/packages/tool-teensy/teensy_loader_cli"
fi

if ! is_bootloader_present; then
    echo "-------------------------------------------------------------------------------"
    echo "Notice: Teensy HalfKay bootloader device (16c0:0478) is not detected."
    echo "If running inside VMware Workstation / Fusion:"
    echo "  1. VMware filters out USB devices with HID keyboard/mouse from guest VMs."
    echo "  2. To flash new firmware:"
    echo "     - Press the physical pushbutton on the Teensy 4.1,"
    echo "     - OR connect a USB-to-UART adapter to Pins 0/1 (RX1/TX1) and pass --uart /dev/ttyUSB0,"
    echo "     - OR add 'usb.generic.allowHID = \"TRUE\"' to your host machine's .vmx file."
    echo "-------------------------------------------------------------------------------"
fi

echo "Flashing hid-multiplexer to Teensy '${BOARD}'..."
flash_success=0
for attempt in 1 2 3; do
    if [[ -n "$TEENSY_LOADER_CLI" ]] && is_bootloader_present; then
        if "$TEENSY_LOADER_CLI" --mcu=TEENSY41 -v -w "$FIRMWARE_HEX"; then
            flash_success=1
            break
        fi
    else
        if pio run -e "$BOARD" -t upload "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}"; then
            flash_success=1
            break
        fi
    fi
    echo "Flash attempt $attempt failed. Retrying in 1s..."
    sleep 1
done

if [[ $flash_success -ne 1 ]]; then
    echo "Error: Failed to flash Teensy after multiple attempts." >&2
    exit 1
fi
