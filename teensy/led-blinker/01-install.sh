#!/bin/bash
#
# Flashes the compiled led-blinker firmware to a Teensy 4.0 or 4.1 board using PlatformIO (teensy-cli).
#
# Usage:
#   ./01-install.sh [-b <teensy41|teensy40>] [upload-options]
#
# Default board is teensy41.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export PATH="$HOME/.local/bin:$HOME/p3/bin:$PATH"

BOARD="teensy41"
EXTRA_ARGS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        -b|--board)
            if [[ $# -lt 2 ]]; then
                echo "Error: -b/--board requires an argument (teensy41 or teensy40)" >&2
                exit 1
            fi
            BOARD="$2"
            shift 2
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
    ./00-build.sh -b "$BOARD"
fi

if ! command -v pio >/dev/null 2>&1; then
    echo "Error: 'pio' (PlatformIO Core) not found in PATH." >&2
    echo "Install it via: pip install platformio" >&2
    exit 1
fi

echo "Flashing led-blinker to Teensy '${BOARD}'..."
echo "Note: If the board does not reboot automatically, press the physical button on the Teensy."
pio run -e "$BOARD" -t upload "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}"
