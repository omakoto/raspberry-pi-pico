#!/bin/bash
#
# Builds and runs test/host_test.cpp, which exercises the virtual matrix and multiplexer on the host
# with USB and flash stubbed out, plus the log ring and the runtime VIAL definition. Run it every time
# touching main/multiplexer.cpp, main/macros.cpp, main/virtual_matrix.cpp, main/device_bindings.cpp, main/log_ring.cpp,
# main/vial_definition.cpp, main/config.h or gen-vial-layout.py.
#
# With --coverage it also reports how much of each main/ source the tests run.

set -euo pipefail

cov_dir="${TMPDIR:-/tmp}/bt-hid-multiplexer-coverage"

usage() {
    cat <<EOF
Usage: test/run-host-test.sh [-c|--coverage] [-h|--help]

Builds the keymap/multiplexer logic, the log ring and the VIAL definition with the host compiler and
runs their tests. Exits with a non-zero status if a test fails.

Options:
  -c, --coverage   Also print the line coverage of each main/ source the tests build. gcov's
                   annotated copies of those sources, with lines that never ran marked "#####", are
                   left in $cov_dir/.
  -h, --help       Show this help.

Examples:
  test/run-host-test.sh
  test/run-host-test.sh --coverage
  grep -n '#####' $cov_dir/multiplexer.cpp.gcov
EOF
}

coverage=0
opts="$(getopt -o ch -l coverage,help -n "$(basename "$0")" -- "$@")" || { usage >&2; exit 1; }
eval set -- "$opts"
while true; do
    case "$1" in
        -c|--coverage) coverage=1; shift ;;
        -h|--help) usage; exit 0 ;;
        --) shift; break ;;
    esac
done
if (( $# > 0 )); then
    usage >&2
    exit 1
fi

cd "$(dirname "$(readlink -f "$0")")"
test_dir="$PWD"
main_dir="$(readlink -f ../main)"

out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT

cflags=(-std=c++17 -Wall -Wextra -Istubs -I../main)
if (( coverage )); then
    cflags+=(--coverage)
fi

# Builds test $1 from the sources that follow (relative to test/) into a directory of its own, where
# the coverage data of its run lands too. Sources are passed by absolute path so that gcov can find
# them from any directory.
build() {
    local name="$1"
    shift
    local srcs=()
    local s
    for s in "$@"; do
        srcs+=("$test_dir/$s")
    done
    mkdir -p "$out/$name"
    g++ "${cflags[@]}" "${srcs[@]}" -o "$out/$name/$name"
}

build host_test host_test.cpp ../main/multiplexer.cpp ../main/virtual_matrix.cpp \
    ../main/device_bindings.cpp ../main/macros.cpp
"$out/host_test/host_test"

build log_ring_test log_ring_test.cpp ../main/log_ring.cpp
"$out/log_ring_test/log_ring_test"

build vial_definition_test vial_definition_test.cpp ../main/vial_definition.cpp
"$out/vial_definition_test/vial_definition_test" "$out/definition.xz"
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

if (( coverage )); then
    rm -rf "$cov_dir"
    mkdir -p "$cov_dir"
    cd "$cov_dir"
    # gcov also reports the tests, the stubs and the C++ library headers; only main/ is of interest.
    gcov "$out"/*/*.gcda 2>/dev/null | awk -v main="$main_dir/" '
        /^File / { file = substr($2, 2, length($2) - 2); next }
        /^Lines executed:/ && index(file, main) == 1 {
            split($2, a, ":"); pct = a[2] + 0; n = $4
            sub(/.*\//, "", file)
            printf "%-24s %6.1f%% of %4d lines\n", file, pct, n
            covered += pct * n / 100; total += n
            file = ""
        }
        END { if (total) printf "%-24s %6.1f%% of %4d lines\n", "Total", covered * 100 / total, total }'
    for f in *.gcov; do
        src="$(sed -n '1s/^.*Source://p' "$f")"
        [[ "$src" == "$main_dir"/* ]] || rm -f "$f"
    done
    echo "Annotated sources (lines that never ran are marked #####): $cov_dir/"
fi
