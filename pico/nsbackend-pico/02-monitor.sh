#!/bin/bash
# Monitors the serial console output of nsbackend-pico over USB CDC ACM or hardware UART.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

PORT=""
BAUD=115200

# Check if port is provided as the first positional argument
if [[ $# -gt 0 && "${1}" =~ ^/dev/tty ]]; then
    PORT="$1"
    shift
fi

# Auto-detect serial port if not explicitly specified
if [[ -z "$PORT" ]]; then
    for candidate in /dev/ttyACM0 /dev/ttyACM1 /dev/ttyUSB0 /dev/ttyUSB1; do
        if [[ -e "$candidate" ]]; then
            PORT="$candidate"
            break
        fi
    done
fi

if [[ -z "$PORT" ]]; then
    echo "Error: No serial port specified or auto-detected (/dev/ttyACM* or /dev/ttyUSB*)." >&2
    echo "Usage: $0 [/dev/ttyACM0] [options]" >&2
    exit 1
fi

echo "Connecting to serial console on ${PORT} at ${BAUD} baud..."

# Try available serial monitor utilities
if command -v tio >/dev/null 2>&1; then
    echo "Launching tio (Press Ctrl+t then q to quit)..."
    exec tio -b "$BAUD" "$PORT" "$@"
elif command -v picocom >/dev/null 2>&1; then
    echo "Launching picocom (Press Ctrl+a then Ctrl+x to quit)..."
    exec picocom -b "$BAUD" "$PORT" "$@"
elif command -v minicom >/dev/null 2>&1; then
    echo "Launching minicom (Press Ctrl+a then q to quit)..."
    exec minicom -D "$PORT" -b "$BAUD" "$@"
elif python3 -c "import serial.tools.miniterm" >/dev/null 2>&1; then
    echo "Launching miniterm (Press Ctrl+] to quit)..."
    exec python3 -m serial.tools.miniterm "$PORT" "$BAUD" "$@"
elif command -v screen >/dev/null 2>&1; then
    echo "Launching screen (Press Ctrl+a then k to quit)..."
    exec screen "$PORT" "$BAUD"
else
    echo "No interactive terminal tool found (tio, picocom, minicom, pyserial, or screen)." >&2
    echo "Streaming raw output with cat (Press Ctrl+C to quit)..."
    stty -F "$PORT" "$BAUD" raw -echo
    cat "$PORT"
fi
