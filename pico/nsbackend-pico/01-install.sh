#!/bin/bash
# Flashes the nsbackend-pico firmware and FATFS storage image to a Raspberry Pi Pico / Pico 2 / Pico W / Pico 2 W.
#
# Supports both:
# 1. Direct copy to mounted BOOTSEL mass-storage drive (e.g., RPI-RP2 or RP2350).
# 2. Automated flashing via picotool.
#
# Note: Flash storage.uf2 before nsbackend-pico.uf2 because flashing the firmware
# triggers an immediate microcontroller reboot into user mode.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Ensure build artifacts exist
FIRMWARE_UF2="build/nsbackend-pico.uf2"

if [[ ! -f "$FIRMWARE_UF2" ]]; then
    echo "Build artifact not found in build/. Running 00-build.sh first..."
    ./00-build.sh
fi

# Locate picotool binary
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

# 2. Attempt to reboot device to BOOTSEL mode via CDC serial console (1200 baud touch and bootloader command)
for cdc_dev in /dev/serial/by-id/usb-HORI_CO._LTD._POKKEN_CONTROLLER*-if01 /dev/ttyACM*; do
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

# 3. Wait up to 6 seconds for the device to appear in BOOTSEL mode
echo "Waiting for Pico in BOOTSEL mode..."
MOUNT_POINT=""

for _ in {1..12}; do
    # Check picotool
    if [[ -n "$PICOTOOL_BIN" ]] && "$PICOTOOL_BIN" info >/dev/null 2>&1; then
        break
    fi

    # Try mounting unmounted RP2350/RPI-RP2 block devices via udisksctl
    if command -v udisksctl >/dev/null 2>&1; then
        for blk_dev in $(lsblk -lno PATH,LABEL 2>/dev/null | grep -E 'RP2350|RPI-RP2' | awk '{print $1}'); do
            udisksctl mount -b "$blk_dev" >/dev/null 2>&1 || true
        done
    fi

    # Check mounted directories
    for candidate in \
        "/media/${USER}/RPI-RP2" \
        "/media/${USER}/RP2350" \
        "/run/media/${USER}/RPI-RP2" \
        "/run/media/${USER}/RP2350" \
        "/mnt/RPI-RP2" \
        "/mnt/RP2350"
    do
        if [[ -d "$candidate" && -w "$candidate" ]]; then
            MOUNT_POINT="$candidate"
            break 2
        fi
    done

    # Check lsblk mountpoint
    DETECTED_PATH="$(lsblk -no MOUNTPOINT -l 2>/dev/null | grep -E 'RPI-RP2|RP2350' | head -n 1 || true)"
    if [[ -n "$DETECTED_PATH" && -d "$DETECTED_PATH" && -w "$DETECTED_PATH" ]]; then
        MOUNT_POINT="$DETECTED_PATH"
        break
    fi

    sleep 0.5
done

# 4. Flash using picotool if available and device is responsive
if [[ -n "$PICOTOOL_BIN" ]] && "$PICOTOOL_BIN" info >/dev/null 2>&1; then
    echo "Found Pico device in BOOTSEL mode via picotool ($PICOTOOL_BIN)."
    echo "Flashing combined firmware and FAT storage image (${FIRMWARE_UF2})..."
    "$PICOTOOL_BIN" load -f -x "$FIRMWARE_UF2"
    echo "Flashing complete. Pico will now reboot."
    exit 0
fi

# 5. Flash via mounted BOOTSEL drive
if [[ -n "$MOUNT_POINT" ]]; then
    echo "Found Pico BOOTSEL drive at: ${MOUNT_POINT}"
    echo "Copying combined firmware and FAT storage image (${FIRMWARE_UF2})..."
    cp "$FIRMWARE_UF2" "${MOUNT_POINT}/"
    sync
    echo "Firmware flashed successfully. Pico will now reboot."
    exit 0
fi

# 6. No device found
echo "Error: No Raspberry Pi Pico board detected in BOOTSEL mode." >&2
echo "" >&2
echo "To put the board into BOOTSEL mode:" >&2
echo "  1. Hold down the BOOTSEL button on the Pico board." >&2
echo "  2. Connect the USB cable to your PC (or press and release the RUN button)." >&2
echo "  3. Release the BOOTSEL button." >&2
echo "  4. Re-run ./01-install.sh, or drag and drop build/*.uf2 directly to the RP2350 / RPI-RP2 volume." >&2
exit 1
