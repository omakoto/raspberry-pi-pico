#!/bin/bash
#
# Builds and runs test/host_test.cpp, which exercises the virtual matrix and multiplexer on the host
# with USB and flash stubbed out. Run it every time touching main/multiplexer.cpp,
# main/virtual_matrix.cpp, main/device_bindings.cpp, main/log_ring.cpp or main/config.h.
#
# Usage: test/run-host-test.sh

set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"

out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT

g++ -std=c++17 -Wall -Wextra -DBOARD_DEVKITC -Istubs -I../main \
    host_test.cpp ../main/multiplexer.cpp ../main/virtual_matrix.cpp ../main/device_bindings.cpp -o "$out/host_test"
"$out/host_test"

g++ -std=c++17 -Wall -Wextra -DBOARD_DEVKITC -Istubs -I../main \
    log_ring_test.cpp ../main/log_ring.cpp -o "$out/log_ring_test"
"$out/log_ring_test"
