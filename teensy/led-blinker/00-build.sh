#!/bin/bash
#
# Builds the led-blinker firmware for Teensy 4.x using PlatformIO.
#
# Usage:
#   ./00-build.sh [-b <teensy41|teensy40>] [-c] [platformio-build-options]
#
# Default board is teensy41.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export PATH="$HOME/.local/bin:$HOME/p3/bin:$PATH"

BOARD="teensy41"
DO_CLEAN=0
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
    echo "Install it via: pip install platformio" >&2
    exit 1
fi

if [[ $DO_CLEAN -eq 1 ]]; then
    echo "Cleaning build directory for environment '${BOARD}'..."
    pio run -e "$BOARD" -t clean
fi

echo "Building led-blinker for board '${BOARD}'..."
pio run -e "$BOARD" "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}"

echo "Build successful! Target outputs generated in .pio/build/${BOARD}/:"
ls -lh ".pio/build/${BOARD}/firmware.hex" ".pio/build/${BOARD}/firmware.elf"
