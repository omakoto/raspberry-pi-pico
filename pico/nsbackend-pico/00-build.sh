#!/bin/bash
#
# Builds the nsbackend-pico firmware and FAT partition image for Raspberry Pi Pico boards.
#
# Usage:
#   ./00-build.sh [-b <pico2_w|pico2|pico_w|pico>] [-c] [cmake-build-options]
#
# Default board is pico2_w.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export PATH="$HOME/.local/bin:$PATH"

export PICO_SDK_PATH="${PICO_SDK_PATH:-$HOME/pico-sdk}"
export FREERTOS_KERNEL_PATH="${FREERTOS_KERNEL_PATH:-$HOME/FreeRTOS-Kernel}"

BOARD="pico2_w"
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

if [[ $DO_CLEAN -eq 1 && -d "build" ]]; then
    echo "Cleaning build directory..."
    rm -rf build
elif [[ -f "build/CMakeCache.txt" ]]; then
    # CMake cannot switch between RP2040 and RP2350 platforms in-place without a clean reconfigure.
    CACHED_BOARD="$(grep -E '^PICO_BOARD:' build/CMakeCache.txt | cut -d= -f2 || true)"
    CACHED_PLATFORM="$(grep -E '^PICO_PLATFORM:' build/CMakeCache.txt | cut -d= -f2 || true)"
    case "$BOARD" in
        pico2*) EXPECTED_PLATFORM="rp2350" ;;
        *)      EXPECTED_PLATFORM="rp2040" ;;
    esac
    if [[ "$CACHED_BOARD" != "$BOARD" || "$CACHED_PLATFORM" != *"$EXPECTED_PLATFORM"* ]]; then
        echo "Target board or platform changed (cached: ${CACHED_BOARD:-none}/${CACHED_PLATFORM:-none}, target: ${BOARD}/${EXPECTED_PLATFORM}). Cleaning build directory..."
        rm -rf build
    fi
fi

mkdir -p build

echo "Configuring nsbackend-pico for board '${BOARD}'..."
cmake -B build \
    -DPICO_BOARD="${BOARD}" \
    -DPICO_SDK_PATH="${PICO_SDK_PATH}" \
    -DFREERTOS_KERNEL_PATH="${FREERTOS_KERNEL_PATH}" \
    "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}"

echo "Building nsbackend-pico for board '${BOARD}'..."
cmake --build build -j"$(nproc)"

echo "Build successful! Target outputs generated in build/:"
ls -lh build/nsbackend-pico.elf build/nsbackend-pico.uf2 build/storage.bin build/storage.uf2
