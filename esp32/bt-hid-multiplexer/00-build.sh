#!/bin/bash
#
# Builds the ESP32-S3 BLE HID Multiplexer firmware with ESP-IDF.
#
# BTstack is compiled from $BTSTACK_ROOT (default: ~/pico-sdk/lib/btstack, the same BTstack the Pico
# build of this firmware uses); see components/btstack/CMakeLists.txt.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

usage() {
    cat <<'EOF'
Usage: 00-build.sh [options] [-- extra idf.py arguments]

Builds the firmware into build/.

Options:
  -D NAME=VALUE       Passes a CMake definition, e.g. -D USB_SERIAL_ALWAYS=ON to always
                      include the USB serial port.
  -c, --clean         Deletes build/ (and sdkconfig) before building.
  -h, --help          Shows this help.

Environment:
  IDF_PATH            ESP-IDF to use (default: ~/esp-idf).
  BTSTACK_ROOT        BTstack tree to compile (default: ~/pico-sdk/lib/btstack).

Examples:
  ./00-build.sh                       # build (the image runs on the XIAO and the DevKitC)
  ./00-build.sh -D USB_SERIAL_ALWAYS=ON
  ./00-build.sh -D USB_ID_MICROSOFT=OFF # use Espressif's USB ID (303a:4004), not Microsoft's
  ./00-build.sh -c                    # clean build
EOF
}

OPTS=$(getopt -o D:ch --long clean,help -n "$(basename "$0")" -- "$@") || { usage >&2; exit 1; }
eval set -- "$OPTS"

DO_CLEAN=0
CMAKE_DEFS=()
while true; do
    case "$1" in
        -D) CMAKE_DEFS+=("-D$2"); shift 2 ;;
        -c|--clean) DO_CLEAN=1; shift ;;
        -h|--help) usage; exit 0 ;;
        --) shift; break ;;
        *) echo "Internal error parsing options" >&2; exit 1 ;;
    esac
done

export BTSTACK_ROOT="${BTSTACK_ROOT:-$HOME/pico-sdk/lib/btstack}"
if [[ ! -f "$BTSTACK_ROOT/src/btstack.h" ]]; then
    echo "Error: BTstack not found at '$BTSTACK_ROOT'. Set BTSTACK_ROOT." >&2
    exit 1
fi

# Activate ESP-IDF environment
IDF_PATH="${IDF_PATH:-$HOME/esp-idf}"
if [[ -f "${IDF_PATH}/export.sh" ]]; then
    # Reset any stale IDF python environment variables
    unset IDF_PYTHON_ENV_PATH
    # shellcheck source=/dev/null
    . "${IDF_PATH}/export.sh" >/dev/null 2>&1
else
    echo "Error: ESP-IDF not found at '${IDF_PATH}'. Please set IDF_PATH or install ESP-IDF." >&2
    exit 1
fi

cd "$SCRIPT_DIR"

if [[ $DO_CLEAN -eq 1 ]]; then
    echo "Cleaning build directory..."
    rm -rf build sdkconfig sdkconfig.old
fi

idf.py "${CMAKE_DEFS[@]+"${CMAKE_DEFS[@]}"}" build "$@"

echo "Build successful: build/bt-hid-multiplexer.bin"
