#!/bin/bash
# deploy.sh: install or upgrade button-dispatcher on a Raspberry Pi, from this PC, over SSH.
#
# Copies this directory to the Pi and runs install.sh there (with sudo, so you may be asked for
# the Pi user's password once). The Pi needs SSH enabled and an internet connection for the
# first install (apt and pip). Running it again upgrades in place and keeps the Pi's config.
#
# Usage:
#   deploy.sh [options] [USER@]HOST
#
# Options:
#   -p, --port PORT   port for the Pi's web page (default 80)
#   -h, --help        show this help
#
# Examples:
#   ./deploy.sh pi9.local
#   ./deploy.sh omakoto@pi9.local --port 8080

set -euo pipefail

SRC_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

usage() {
    sed -n '2,/^$/{s/^# \{0,1\}//;p}' "${BASH_SOURCE[0]}"
}

die() {
    echo "deploy.sh: $*" >&2
    exit 1
}

port=80
opts=$(getopt -o p:h --long port:,help -n deploy.sh -- "$@") || exit 2
eval set -- "$opts"
while true; do
    case "$1" in
        -p|--port) port=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        --) shift; break ;;
        *) die "internal error parsing options" ;;
    esac
done
[[ $# -eq 1 ]] || die "give exactly one host, like pi9.local or user@pi9.local (see --help)"
host=$1
[[ $port =~ ^[0-9]+$ ]] || die "invalid port: $port"
[[ -d $SRC_DIR/button_dispatcher ]] || die "button_dispatcher/ not found next to this script"

# The installer runs from a throwaway directory on the Pi; -t lets sudo ask for a password.
remote_dir=/tmp/button-dispatcher-install
echo "==> Copying to $host"
tar -C "$SRC_DIR" --exclude='__pycache__' --exclude='*.pyc' --exclude='.git' -cf - . |
    ssh "$host" "rm -rf $remote_dir && mkdir -p $remote_dir && tar -C $remote_dir -xf -"
echo "==> Installing on $host"
status=0
ssh -t "$host" "sudo $remote_dir/install.sh --port $port" || status=$?
ssh "$host" "rm -rf $remote_dir" || true
exit $status
