#!/bin/bash
#
# Builds the HID multiplexer firmware for Teensy 4.1 using PlatformIO.
#
# Usage:
#   ./00-build.sh [-c] [platformio-build-options]
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export PATH="$HOME/.local/bin:$HOME/p3/bin:$PATH"

BOARD="teensy41"
DO_CLEAN=0
EXTRA_ARGS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        -c|--clean)
            DO_CLEAN=1
            shift
            ;;
        *)
            EXTRA_ARGS+=("$1")
            shift
            ;;
    esac
done

cd "$SCRIPT_DIR"

if ! command -v pio >/dev/null 2>&1; then
    echo "Error: 'pio' (PlatformIO Core) not found in PATH." >&2
    echo "Install it via /home/omakoto/cbin/setup/install-teensy" >&2
    exit 1
fi

if [[ $DO_CLEAN -eq 1 ]]; then
    echo "Cleaning build directory for environment '${BOARD}'..."
    pio run -e "$BOARD" -t clean
fi

# Ensure Teensyduino core has the WebHID EP7 RawHID patch applied
CORE_DESC="$HOME/.platformio/packages/framework-arduinoteensy/cores/teensy4/usb_desc.h"
PATCH_FILE="$SCRIPT_DIR/patches/teensy4_usb_desc_ep7_rawhid.patch"
if [[ -f "$CORE_DESC" && -f "$PATCH_FILE" ]] && ! grep -q "WebHID VIAL" "$CORE_DESC"; then
    echo "Applying EP7 WebHID RawHID patch to Teensyduino core ($CORE_DESC)..."
    patch -p1 -d "$HOME/.platformio/packages/framework-arduinoteensy" < "$PATCH_FILE"
fi

echo "Building hid-multiplexer for board '${BOARD}'..."
pio run -e "$BOARD" "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}"

echo "Build successful! Target outputs generated in .pio/build/${BOARD}/:"
ls -lh ".pio/build/${BOARD}/firmware.hex" ".pio/build/${BOARD}/firmware.elf"
