#!/bin/bash
# Test for circuit-run
#
# Simple script, no help needed.

set -e

# Create a temporary directory for mocking tools and mount point
TEMP_DIR=$(mktemp -d)
trap 'rm -rf "$TEMP_DIR"' EXIT

# Directories to mock
BIN_DIR="$TEMP_DIR/bin"
MOCK_MOUNT="$TEMP_DIR/mock_mount"
mkdir -p "$BIN_DIR"
mkdir -p "$MOCK_MOUNT"

# Create mock find-circut-dir
MOCK_FIND_DIR="$BIN_DIR/find-circut-dir"
cat << EOF > "$MOCK_FIND_DIR"
#!/bin/bash
if [[ -n "\$MOCK_CIRCUIT_DIR" ]]; then
  echo "\$MOCK_CIRCUIT_DIR"
  exit 0
else
  exit 1
fi
EOF
chmod +x "$MOCK_FIND_DIR"

# Copy the actual circuit-run to our temporary bin directory so it resolves find-circut-dir relative to itself
cp ./bin/circuit-run "$BIN_DIR/circuit-run"

# Create a source file to copy
SRC_FILE="$TEMP_DIR/test_script.py"
echo "print('Hello CircuitPython')" > "$SRC_FILE"

# Test 1: Successful copy
export MOCK_CIRCUIT_DIR="$MOCK_MOUNT"
"$BIN_DIR/circuit-run" "$SRC_FILE"

# Verify file was copied as code.py
DEST_FILE="$MOCK_MOUNT/code.py"
if [[ ! -f "$DEST_FILE" ]]; then
  echo "FAIL: Destination file $DEST_FILE not found"
  exit 1
fi

dest_content=$(cat "$DEST_FILE")
expected_content="import supervisor; supervisor.runtime.autoreload = False;print('Hello CircuitPython')"
if [[ "$dest_content" != "$expected_content" ]]; then
  echo "FAIL: Destination file content incorrect: '$dest_content'"
  exit 1
fi

# Test 2: Failure when find-circut-dir fails
unset MOCK_CIRCUIT_DIR
if "$BIN_DIR/circuit-run" "$SRC_FILE" 2>/dev/null; then
  echo "FAIL: Expected circuit-run to fail when find-circut-dir fails"
  exit 1
fi

# Test 3: Failure when source file does not exist
export MOCK_CIRCUIT_DIR="$MOCK_MOUNT"
if "$BIN_DIR/circuit-run" "$TEMP_DIR/nonexistent.py" 2>/dev/null; then
  echo "FAIL: Expected circuit-run to fail when source file does not exist"
  exit 1
fi

# Test 4: Failure when mount directory does not exist or is not a directory
export MOCK_CIRCUIT_DIR="$TEMP_DIR/nonexistent_mount"
if "$BIN_DIR/circuit-run" "$SRC_FILE" 2>/dev/null; then
  echo "FAIL: Expected circuit-run to fail when mount directory does not exist"
  exit 1
fi

# Test 5: Copy tagged files using #file: comments
export MOCK_CIRCUIT_DIR="$MOCK_MOUNT"
SRC_FILE_TAGGED="$TEMP_DIR/test_script_tagged.py"
EXTRA_FILE="$TEMP_DIR/extra_file.txt"

echo "print('Hello')" > "$SRC_FILE_TAGGED"
echo "#file:extra_file.txt" >> "$SRC_FILE_TAGGED"
echo "data" > "$EXTRA_FILE"

"$BIN_DIR/circuit-run" "$SRC_FILE_TAGGED"

# Verify extra_file.txt was copied
EXTRA_DEST_FILE="$MOCK_MOUNT/extra_file.txt"
if [[ ! -f "$EXTRA_DEST_FILE" ]]; then
  echo "FAIL: Extra file $EXTRA_DEST_FILE was not copied"
  exit 1
fi

extra_content=$(cat "$EXTRA_DEST_FILE")
if [[ "$extra_content" != "data" ]]; then
  echo "FAIL: Extra file content incorrect: '$extra_content'"
  exit 1
fi

echo "All tests passed!"
