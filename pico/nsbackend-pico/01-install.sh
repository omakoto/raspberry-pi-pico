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
STORAGE_UF2="build/storage.uf2"

if [[ ! -f "$FIRMWARE_UF2" || ! -f "$STORAGE_UF2" ]]; then
    echo "Build artifacts not found in build/. Running 00-build.sh first..."
    ./00-build.sh
fi

echo "Looking for connected Raspberry Pi Pico in BOOTSEL mode..."

# 1. Search for mounted BOOTSEL drive (RPI-RP2 or RP2350)
MOUNT_POINT=""
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
        break
    fi
done

# If not found in standard mount paths, check lsblk / mount output
if [[ -z "$MOUNT_POINT" ]]; then
    DETECTED_PATH="$(lsblk -no MOUNTPOINT -l 2>/dev/null | grep -E 'RPI-RP2|RP2350' | head -n 1 || true)"
    if [[ -n "$DETECTED_PATH" && -d "$DETECTED_PATH" && -w "$DETECTED_PATH" ]]; then
        MOUNT_POINT="$DETECTED_PATH"
    fi
fi

# Flash via mounted volume if found
if [[ -n "$MOUNT_POINT" ]]; then
    echo "Found Pico BOOTSEL drive at: ${MOUNT_POINT}"
    echo "Copying FAT storage image (${STORAGE_UF2})..."
    cp "$STORAGE_UF2" "${MOUNT_POINT}/"
    sync

    # Allow drive to process storage image if still mounted
    sleep 1

    if [[ -d "$MOUNT_POINT" ]]; then
        echo "Copying firmware binary (${FIRMWARE_UF2})..."
        cp "$FIRMWARE_UF2" "${MOUNT_POINT}/"
        sync
        echo "Firmware flashed successfully. Pico will now reboot."
        exit 0
    else
        echo "Device rebooted after storage flash. Please put the board back into BOOTSEL mode to flash firmware."
        exit 0
    fi
fi

# 2. Search for picotool
PICOTOOL_BIN="$(command -v picotool || true)"
if [[ -z "$PICOTOOL_BIN" && -x "$HOME/.local/bin/picotool" ]]; then
    PICOTOOL_BIN="$HOME/.local/bin/picotool"
fi
if [[ -z "$PICOTOOL_BIN" && -x "build/_deps/picotool-build/picotool" ]]; then
    PICOTOOL_BIN="build/_deps/picotool-build/picotool"
fi

if [[ -n "$PICOTOOL_BIN" ]]; then
    if "$PICOTOOL_BIN" info >/dev/null 2>&1; then
        echo "Found Pico device via picotool ($PICOTOOL_BIN)."
        echo "Flashing FAT storage image..."
        "$PICOTOOL_BIN" load -f "$STORAGE_UF2"
        echo "Flashing firmware and rebooting..."
        "$PICOTOOL_BIN" load -f -x "$FIRMWARE_UF2"
        echo "Flashing complete."
        exit 0
    fi
fi

# 3. No device found
echo "Error: No Raspberry Pi Pico board detected in BOOTSEL mode." >&2
echo "" >&2
echo "To put the board into BOOTSEL mode:" >&2
echo "  1. Hold down the BOOTSEL button on the Pico board." >&2
echo "  2. Connect the USB cable to your PC (or press and release the RUN button)." >&2
echo "  3. Release the BOOTSEL button." >&2
echo "  4. Re-run ./01-install.sh, or drag and drop build/*.uf2 directly to the RPI-RP2 volume." >&2
exit 1
