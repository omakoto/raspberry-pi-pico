#!/bin/bash
# Builds the nsbackend-esp32s3 firmware and FATFS filesystem image for ESP32-S3 using ESP-IDF.

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
echo "Building nsbackend-esp32s3 in ${SCRIPT_DIR}..."
idf.py build "$@"
