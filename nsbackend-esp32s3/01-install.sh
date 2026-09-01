#!/bin/bash
# Flashes the compiled nsbackend-esp32s3 firmware, partition table, and SPIFFS image to the ESP32-S3.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

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

# Ensure binaries exist; if not, trigger build first
if [[ ! -f "build/nsbackend-esp32s3.bin" || ! -f "build/storage.bin" ]]; then
    echo "Binaries not found. Running 00-build.sh first..."
    ./00-build.sh
fi

PORT="${ESPPORT:-}"

# Check if port is provided as the first positional argument
if [[ $# -gt 0 && "${1}" =~ ^/dev/tty ]]; then
    PORT="$1"
    shift
fi

# Auto-detect serial port if not explicitly set
if [[ -z "$PORT" ]]; then
    for candidate in /dev/ttyACM0 /dev/ttyACM1 /dev/ttyUSB0 /dev/ttyUSB1; do
        if [[ -e "$candidate" ]]; then
            PORT="$candidate"
            break
        fi
    done
fi

if [[ -n "$PORT" ]]; then
    echo "Flashing nsbackend-esp32s3 to port ${PORT}..."
    idf.py -p "$PORT" flash "$@"
else
    echo "No serial port specified or auto-detected. Attempting default flash..."
    idf.py flash "$@"
fi
