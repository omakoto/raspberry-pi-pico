#!/bin/bash
#
# Builds and runs test/host_test.cpp, which exercises the virtual matrix and multiplexer on the host
# with USB and flash stubbed out, plus the log ring and the runtime VIAL definition. Run it every time
# touching main/multiplexer.cpp, main/virtual_matrix.cpp, main/device_bindings.cpp, main/log_ring.cpp,
# main/vial_definition.cpp, main/config.h or gen-vial-layout.py.
#
# Usage: test/run-host-test.sh

set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"

out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT

g++ -std=c++17 -Wall -Wextra -Istubs -I../main \
    host_test.cpp ../main/multiplexer.cpp ../main/virtual_matrix.cpp ../main/device_bindings.cpp -o "$out/host_test"
"$out/host_test"

g++ -std=c++17 -Wall -Wextra -Istubs -I../main \
    log_ring_test.cpp ../main/log_ring.cpp -o "$out/log_ring_test"
"$out/log_ring_test"

g++ -std=c++17 -Wall -Wextra -Istubs -I../main \
    vial_definition_test.cpp ../main/vial_definition.cpp -o "$out/vial_definition_test"
"$out/vial_definition_test" "$out/definition.xz"
# VIAL reads the definition with Python's lzma and json modules; so does this check.
python3 - "$out/definition.xz" <<'PYEOF'
import json, lzma, sys
d = json.loads(lzma.decompress(open(sys.argv[1], "rb").read()))
labels = d["layouts"]["labels"]
choices = ["No binding"] + [f"Layer {n}" for n in range(1, 8)]
assert labels == [["Keychron Nape Pro"] + choices,
                  ['Quote" Back\\slash ???'] + choices,
                  ["D6:54:CB:91:59:74"] + choices,
                  ["MX Dialpad (7B:44)"] + choices,
                  ["MX Dialpad (05:06)"] + choices], labels
assert d["matrix"] == {"rows": 16, "cols": 16} and d["layouts"]["keymap"], d
print("VIAL definition decodes: OK")
PYEOF
