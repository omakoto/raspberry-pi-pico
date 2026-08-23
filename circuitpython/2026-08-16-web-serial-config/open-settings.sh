#!/bin/bash
# Open settings.html located on the connected CircuitPython device in Chrome using 'c'.

set -e

DIR="$(dirname "${BASH_SOURCE[0]}")"

# Find the CircuitPython mount directory
if command -v find-circut-dir >/dev/null 2>&1; then
  find_cmd="find-circut-dir"
elif [[ -x "$DIR/../bin/find-circut-dir" ]]; then
  find_cmd="$DIR/../bin/find-circut-dir"
else
  echo "Error: find-circut-dir command not found." >&2
  exit 1
fi

if ! circuit_dir=$("$find_cmd" 2>/dev/null); then
  echo "Error: CircuitPython mount directory not found. Is the Raspberry Pi Pico plugged in?" >&2
  exit 1
fi

settings_file="$circuit_dir/settings.html"

if [[ ! -f "$settings_file" ]]; then
  echo "Error: settings.html not found on the device at '$settings_file'." >&2
  echo "Hint: Run ./code.py or copy settings.html to the CIRCUITPY drive first." >&2
  exit 1
fi

echo "Opening $settings_file..."
c "$settings_file" "$@"
