#!/bin/bash
#
# Builds the nsbackend-pico firmware and FAT partition image for Raspberry Pi Pico boards.
#
# Usage:
#   ./00-build.sh [-b <pico2_w|pico2>] [-c] [cmake-build-options]
#
# Default board is pico2_w.
#
# Only RP2350 boards are supported. The RP2040 boards (pico, pico_w) are rejected because
# the firmware's static footprint does not fit in their 264KB of SRAM.
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

# Reject RP2040 boards up front so the failure is obvious rather than a linker overflow.
if [[ "$BOARD" == "pico" || "$BOARD" == "pico_w" ]]; then
    cat >&2 <<EOF
Error: board '${BOARD}' is an RP2040 board and is not supported.

This firmware needs the RP2350's 512KB of SRAM; the RP2040 has 264KB and the
image does not link. Build for an RP2350 board instead:

    ./00-build.sh -b pico2_w    # Pico 2 W (default)
    ./00-build.sh -b pico2      # Pico 2, no Wi-Fi
EOF
    exit 1
fi

if [[ $DO_CLEAN -eq 1 && -d "build" ]]; then
    echo "Cleaning build directory..."
    rm -rf build
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
