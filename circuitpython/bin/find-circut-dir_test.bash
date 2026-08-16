#!/bin/bash
# Test for find-circut-dir
#
# Simple script, no help needed.

set -e

# Create a temporary directory for mocking findmnt
TEMP_DIR=$(mktemp -d)
trap 'rm -rf "$TEMP_DIR"' EXIT

# Create a mock findmnt executable
MOCK_FINDMNT="$TEMP_DIR/findmnt"
cat << 'EOF' > "$MOCK_FINDMNT"
#!/bin/bash
if [[ "$*" == *"-S LABEL=CIRCUITPY"* ]]; then
  if [[ -n "$MOCK_CIRCUITPY_MOUNTS" ]]; then
    echo -e "$MOCK_CIRCUITPY_MOUNTS"
    exit 0
  else
    exit 1
  fi
fi
exit 1
EOF
chmod +x "$MOCK_FINDMNT"

# Prepend the temp directory to PATH to override findmnt
export PATH="$TEMP_DIR:$PATH"

# Test 1: Single mount point found
export MOCK_CIRCUITPY_MOUNTS="/media/user/CIRCUITPY"
output=$(./bin/find-circut-dir)
if [[ "$output" != "/media/user/CIRCUITPY" ]]; then
  echo "FAIL: Expected /media/user/CIRCUITPY, got $output"
  exit 1
fi

# Test 2: Multiple mount points found (should return the first one)
export MOCK_CIRCUITPY_MOUNTS="/media/user/CIRCUITPY-1
/media/user/CIRCUITPY-2"
output=$(./bin/find-circut-dir)
if [[ "$output" != "/media/user/CIRCUITPY-1" ]]; then
  echo "FAIL: Expected /media/user/CIRCUITPY-1, got $output"
  exit 1
fi

# Test 3: No mount point found (should exit with 1, print error to stderr, and no stdout)
unset MOCK_CIRCUITPY_MOUNTS
stdout_file="$TEMP_DIR/stdout"
stderr_file="$TEMP_DIR/stderr"

if ./bin/find-circut-dir > "$stdout_file" 2> "$stderr_file"; then
  echo "FAIL: Expected exit code 1 when no mounts found"
  exit 1
fi

stdout_output=$(cat "$stdout_file")
stderr_output=$(cat "$stderr_file")

if [[ -n "$stdout_output" ]]; then
  echo "FAIL: Expected empty stdout, got: $stdout_output"
  exit 1
fi

if [[ "$stderr_output" != "CircuitPython mount directory not found" ]]; then
  echo "FAIL: Expected stderr 'CircuitPython mount directory not found', got: '$stderr_output'"
  exit 1
fi

echo "All tests passed!"
