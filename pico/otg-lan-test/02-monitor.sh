#!/bin/bash
# Monitors the otg-lan-test log output, normally on the USB CDC serial console
# (/dev/ttyACM*) that the board exposes next to its network interface. The same log is
# also on UART0 (GP0 TX / GP1 RX, 115200 baud) for a USB-to-serial adapter.
#
# Usage: ./02-monitor.sh [/dev/ttyACM0] [extra args for the terminal program]

set -euo pipefail

PORT=""
BAUD=115200

if [[ $# -gt 0 && "${1}" =~ ^/dev/tty ]]; then
    PORT="$1"
    shift
fi

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
