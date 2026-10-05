#!/bin/bash
#
# Builds and runs test/host_test.cpp, which exercises the virtual matrix and multiplexer on the host
# with USB and flash stubbed out. Run it every time touching src/multiplexer.cpp,
# src/virtual_matrix.cpp, src/device_bindings.cpp, src/log_ring.cpp or src/config.h.
#
# Usage: test/run-host-test.sh

set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"

out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT

g++ -std=c++17 -Wall -Wextra -Istubs -I../src \
    host_test.cpp ../src/multiplexer.cpp ../src/virtual_matrix.cpp ../src/device_bindings.cpp -o "$out/host_test"
"$out/host_test"

g++ -std=c++17 -Wall -Wextra -Istubs -I../src \
    log_ring_test.cpp ../src/log_ring.cpp -o "$out/log_ring_test"
"$out/log_ring_test"
