#!/bin/bash
# run.sh: build and run the host unit tests for dispatcher_core on this PC (no ESP32 needed).
#
# Compiles components/dispatcher_core/src/*.cpp and test/test_*.cpp with g++, plus ESP-IDF's own cJSON
# (the same library the firmware uses), into /tmp and runs the result. The code is built with
# -fno-exceptions -fno-rtti, like the firmware, so accidental use of either is a compile error, and with
# AddressSanitizer and UndefinedBehaviorSanitizer so leaks and undefined behavior fail the run.
# The exit status is the tests' exit status.
#
# Options:
#   -n, --no-sanitize   build without the sanitizers (faster to compile)
#   -h, --help          show this help
#
# Examples:
#   test/run.sh
#   IDF_PATH=~/esp-idf test/run.sh --no-sanitize

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
CORE_DIR="$PROJECT_DIR/components/dispatcher_core"
IDF_PATH="${IDF_PATH:-$HOME/esp-idf}"
CJSON_DIR="$IDF_PATH/components/json/cJSON"
BUILD_DIR=/tmp/button-dispatcher-core-test

usage() {
    sed -n '2,/^$/{s/^# \{0,1\}//;p}' "${BASH_SOURCE[0]}"
}

sanitize=1
opts=$(getopt -o hn --long help,no-sanitize -n run.sh -- "$@") || exit 2
eval set -- "$opts"
while true; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        -n|--no-sanitize) sanitize=0; shift ;;
        --) shift; break ;;
        *) echo "run.sh: internal error" >&2; exit 2 ;;
    esac
done

[[ -f $CJSON_DIR/cJSON.c ]] || { echo "run.sh: cJSON not found at $CJSON_DIR (set IDF_PATH)" >&2; exit 1; }

mkdir -p "$BUILD_DIR"
sanitizer_flags=()
if [[ $sanitize -eq 1 ]]; then
    sanitizer_flags=(-fsanitize=address,undefined -fno-sanitize-recover=undefined)
fi
cxxflags=(-std=c++17 -Wall -Wextra -Werror -O1 -g -fno-exceptions -fno-rtti -pthread "${sanitizer_flags[@]}"
          -I"$CORE_DIR/include" -I"$CORE_DIR/src" -I"$SCRIPT_DIR" -I"$CJSON_DIR")

# cJSON is third-party C code, so it is built without -Werror.
gcc -O1 -g "${sanitizer_flags[@]}" -c "$CJSON_DIR/cJSON.c" -I"$CJSON_DIR" -o "$BUILD_DIR/cJSON.o"

sources=("$CORE_DIR"/src/*.cpp "$SCRIPT_DIR"/test_*.cpp)
g++ "${cxxflags[@]}" "${sources[@]}" "$BUILD_DIR/cJSON.o" -o "$BUILD_DIR/tests"
"$BUILD_DIR/tests"
