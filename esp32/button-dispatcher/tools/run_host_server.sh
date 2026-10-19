#!/bin/bash
# run_host_server.sh: build and run tools/host_server.cpp, the firmware's core behind a real HTTP server on this
# PC, so the web page can be tried against the C++ implementation without an ESP32.
#
# Everything after the options is passed to the server (see the top of tools/host_server.cpp), for example
# --port 8097 --data-dir /tmp/bd-host. There is no Bluetooth; GET /test/packet injects advertisements.
#
# Options:
#   -b, --build-only   build the server and print its path instead of running it
#   -h, --help         show this help
#
# Examples:
#   tools/run_host_server.sh --port 8097
#   tools/run_host_server.sh --build-only

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
CORE_DIR="$PROJECT_DIR/components/dispatcher_core"
IDF_PATH="${IDF_PATH:-$HOME/esp-idf}"
CJSON_DIR="$IDF_PATH/components/json/cJSON"
BUILD_DIR=/tmp/button-dispatcher-host-build

usage() {
    sed -n '2,/^$/{s/^# \{0,1\}//;p}' "${BASH_SOURCE[0]}"
}

build_only=0
opts=$(getopt -o +bh --long build-only,help -n run_host_server.sh -- "$@") || exit 2
eval set -- "$opts"
while true; do
    case "$1" in
        -b|--build-only) build_only=1; shift ;;
        -h|--help) usage; exit 0 ;;
        --) shift; break ;;
        *) echo "run_host_server.sh: internal error" >&2; exit 2 ;;
    esac
done

[[ -f $CJSON_DIR/cJSON.c ]] || { echo "run_host_server.sh: cJSON not found at $CJSON_DIR (set IDF_PATH)" >&2; exit 1; }
mkdir -p "$BUILD_DIR"
gcc -O1 -g -c "$CJSON_DIR/cJSON.c" -I"$CJSON_DIR" -o "$BUILD_DIR/cJSON.o"
g++ -std=c++17 -Wall -Wextra -O1 -g -pthread -I"$CORE_DIR/include" -I"$CORE_DIR/src" -I"$PROJECT_DIR/test" \
    -I"$CJSON_DIR" "$SCRIPT_DIR/host_server.cpp" "$CORE_DIR"/src/*.cpp "$BUILD_DIR/cJSON.o" -o "$BUILD_DIR/host_server"

if [[ $build_only -eq 1 ]]; then
    echo "$BUILD_DIR/host_server"
    exit 0
fi
cd "$PROJECT_DIR"
exec "$BUILD_DIR/host_server" "$@"
