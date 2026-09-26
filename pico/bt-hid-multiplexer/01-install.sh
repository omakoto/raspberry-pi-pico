#!/bin/bash
#
# Flashes the bt-hid-multiplexer firmware to a Raspberry Pi Pico 2 W / Pico W board.
#
# Supports automated flashing via picotool or direct copy to mounted BOOTSEL mass-storage drive.
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

echo "Checking for connected Pico board..."

# Attempt picotool load if available
if [[ -n "$PICOTOOL_BIN" ]]; then
    if "$PICOTOOL_BIN" info >/dev/null 2>&1; then
        echo "Pico detected in BOOTSEL mode via picotool. Flashing..."
        "$PICOTOOL_BIN" load -x "$FIRMWARE_UF2"
        echo "Flashing successful!"
        exit 0
    fi
fi

# Check for mounted BOOTSEL volume
BOOTSEL_DIRS=(/media/*/* /run/media/*/* /mnt/*)
for dir in "${BOOTSEL_DIRS[@]}"; do
    if [[ -d "$dir" && (-f "$dir/INFO_UF2.TXT" || -f "$dir/info_uf2.txt") ]]; then
        echo "Found mounted BOOTSEL drive at '$dir'. Copying $FIRMWARE_UF2..."
        cp "$FIRMWARE_UF2" "$dir/"
        sync
        echo "Firmware copied! Pico will reboot into user mode."
        exit 0
    fi
done

echo "Error: No connected Pico in BOOTSEL mode detected." >&2
echo "Please hold the BOOTSEL button while plugging in the Pico, or run:" >&2
echo "  picotool reboot -f -u" >&2
exit 1
