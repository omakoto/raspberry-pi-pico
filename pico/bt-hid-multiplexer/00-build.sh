#!/bin/bash
#
# Builds the Pico 2 W BLE HID Multiplexer firmware.
#
# Usage:
#   ./00-build.sh [-b <pico2_w|pico_w>] [-c|--clean]
#
# Default target board is pico2_w.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export PATH="$HOME/.local/bin:$PATH"

export PICO_SDK_PATH="${PICO_SDK_PATH:-$HOME/pico-sdk}"

BOARD="${PICO_BOARD:-}"
DO_CLEAN=0
EXTRA_ARGS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        -b|--board)
            if [[ $# -lt 2 ]]; then
                echo "Error: -b/--board requires an argument" >&2
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

if [[ -z "$BOARD" ]]; then
    if [[ -f "build/CMakeCache.txt" ]]; then
        BOARD="$(grep -E '^PICO_BOARD:' build/CMakeCache.txt | cut -d= -f2 || true)"
    fi
    BOARD="${BOARD:-pico_w}"
fi

if [[ $DO_CLEAN -eq 1 && -d "build" ]]; then
    echo "Cleaning build directory..."
    rm -rf build
elif [[ -f "build/CMakeCache.txt" ]]; then
    CACHED_BOARD="$(grep -E '^PICO_BOARD:' build/CMakeCache.txt | cut -d= -f2 || true)"
    if [[ "$CACHED_BOARD" != "$BOARD" ]]; then
        echo "Target board changed (cached: ${CACHED_BOARD:-none}, target: ${BOARD}). Cleaning build directory..."
        rm -rf build
    fi
fi

mkdir -p build

echo "Configuring bt-hid-multiplexer for board '${BOARD}'..."
cmake -B build \
    -DPICO_BOARD="${BOARD}" \
    -DPICO_SDK_PATH="${PICO_SDK_PATH}" \
    "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}"

echo "Building bt-hid-multiplexer for board '${BOARD}'..."
cmake --build build -j"$(nproc)"

echo "Build successful! Target outputs generated in build/:"
ls -lh build/bt-hid-multiplexer.elf build/bt-hid-multiplexer.uf2
