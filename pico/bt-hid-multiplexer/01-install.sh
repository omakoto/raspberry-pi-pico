#!/bin/bash
#
# Flashes the bt-hid-multiplexer firmware to a Raspberry Pi Pico 2 W / Pico W board.
#
# Automatically drops the board into BOOTSEL mode via USB CDC 1200-baud touch or
# UART console without requiring physical button presses or manual mode switching.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

FIRMWARE_UF2="build/bt-hid-multiplexer.uf2"

if [[ ! -f "$FIRMWARE_UF2" ]]; then
    echo "Firmware binary not found. Running 00-build.sh first..."
    ./00-build.sh
fi

# Locate picotool
PICOTOOL_BIN="$(command -v picotool || true)"
if [[ -z "$PICOTOOL_BIN" && -x "$HOME/.local/bin/picotool" ]]; then
    PICOTOOL_BIN="$HOME/.local/bin/picotool"
fi
if [[ -z "$PICOTOOL_BIN" && -x "build/_deps/picotool-build/picotool" ]]; then
    PICOTOOL_BIN="build/_deps/picotool-build/picotool"
fi

echo "Checking for connected Pico board..."

# 1. Attempt to reboot device to BOOTSEL mode via picotool
if [[ -n "$PICOTOOL_BIN" ]]; then
    echo "Attempting to reset device into BOOTSEL mode via picotool..."
    "$PICOTOOL_BIN" reboot -f -u >/dev/null 2>&1 || true
fi

# 2. Attempt to reboot device to BOOTSEL mode via USB CDC serial console (1200 baud pulse and bootloader command)
for cdc_dev in /dev/serial/by-id/usb-Raspberry_Pi_Pico_2_W_BLE_HID_Multiplexer*-if00 /dev/ttyACM*; do
    if [[ -e "$cdc_dev" ]]; then
        echo "Found active Pico serial console at $cdc_dev. Requesting reboot to BOOTSEL mode..."
        python3 -c "
import os, termios
try:
    fd = os.open('$cdc_dev', os.O_RDWR | os.O_NOCTTY)
    attrs = termios.tcgetattr(fd)
    attrs[4] = termios.B1200
    attrs[5] = termios.B1200
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    try:
        os.write(fd, b'bootloader\r\n')
    except Exception:
        pass
    os.close(fd)
except Exception:
    pass
" 2>/dev/null || true
        break
    fi
done

# 3. Attempt to reboot via hardware UART0 console (GP16/GP17 at 115200 baud)
UART_DEV="${MULTIPLEXER_UART:-${PICO_UART:-}}"
if [[ -n "${UART_DEV}" && -e "${UART_DEV}" ]]; then
    echo "Sending 'bootloader' command over UART console ${UART_DEV}..."
    python3 -c "
import os, termios
try:
    fd = os.open('${UART_DEV}', os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0; attrs[1] = 0; attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL; attrs[3] = 0
    attrs[4] = termios.B115200; attrs[5] = termios.B115200
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    os.write(fd, b'bootloader\r\n')
    os.close(fd)
except Exception:
    pass
" 2>/dev/null || true
fi

# 4. Wait up to 6 seconds for device to appear in BOOTSEL mode
echo "Waiting for Pico in BOOTSEL mode..."
FOUND_BOOTSEL=0

for _ in {1..12}; do
    if [[ -n "$PICOTOOL_BIN" ]] && "$PICOTOOL_BIN" info >/dev/null 2>&1; then
        FOUND_BOOTSEL=1
        break
    fi

    # Check mounted directories
    for dir in /media/*/* /run/media/*/* /mnt/*; do
        if [[ -d "$dir" && (-f "$dir/INFO_UF2.TXT" || -f "$dir/info_uf2.txt") ]]; then
            FOUND_BOOTSEL=1
            break 2
        fi
    done

    sleep 0.5
done

# 5. Flash via picotool if available
if [[ -n "$PICOTOOL_BIN" ]] && "$PICOTOOL_BIN" info >/dev/null 2>&1; then
    echo "Flashing $FIRMWARE_UF2 via picotool..."
    "$PICOTOOL_BIN" load -x "$FIRMWARE_UF2"
    echo "Flashing successful! Microcontroller rebooted into user mode."
    exit 0
fi

# 6. Flash via mounted BOOTSEL mass-storage drive
for dir in /media/*/* /run/media/*/* /mnt/*; do
    if [[ -d "$dir" && (-f "$dir/INFO_UF2.TXT" || -f "$dir/info_uf2.txt") ]]; then
        echo "Found mounted BOOTSEL drive at '$dir'. Copying $FIRMWARE_UF2..."
        cp "$FIRMWARE_UF2" "$dir/"
        sync
        echo "Firmware copied! Microcontroller rebooted into user mode."
        exit 0
    fi
done

echo "Error: Device failed to enter BOOTSEL mode." >&2
echo "Please verify connections, or manually hold the BOOTSEL button while plugging in the board." >&2
exit 1
