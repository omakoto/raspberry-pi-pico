#!/bin/bash
# uninstall.sh: remove button-dispatcher from this Raspberry Pi.
#
# Stops and removes the service and the program. By default the data in /var/lib/button-dispatcher
# (config, SwitchBot credentials, password) and the service user are kept, so a later install picks
# up where this one left off.
#
# Options:
#   --purge     also delete the data directory and the service user
#   -h, --help  show this help
#
# Examples:
#   sudo ./uninstall.sh
#   sudo ./uninstall.sh --purge

set -euo pipefail

APP_DIR=/opt/button-dispatcher
DATA_DIR=/var/lib/button-dispatcher
SERVICE_USER=button-dispatcher
UNIT=/etc/systemd/system/button-dispatcher.service
WRAPPER=/usr/local/bin/button-dispatcher

usage() {
    sed -n '2,/^$/{s/^# \{0,1\}//;p}' "${BASH_SOURCE[0]}"
}

die() {
    echo "uninstall.sh: $*" >&2
    exit 1
}

purge=0
opts=$(getopt -o h --long purge,help -n uninstall.sh -- "$@") || exit 2
eval set -- "$opts"
while true; do
    case "$1" in
        --purge) purge=1; shift ;;
        -h|--help) usage; exit 0 ;;
        --) shift; break ;;
        *) die "internal error parsing options" ;;
    esac
done
[[ $# -eq 0 ]] || die "unexpected argument: $1 (see --help)"

if [[ $EUID -ne 0 ]]; then
    if [[ $purge -eq 1 ]]; then exec sudo "$0" --purge; else exec sudo "$0"; fi
fi

systemctl disable --now button-dispatcher >/dev/null 2>&1 || true
rm -f "$UNIT" "$WRAPPER"
systemctl daemon-reload
rm -rf "$APP_DIR"
echo "Removed the service and $APP_DIR."

if [[ $purge -eq 1 ]]; then
    rm -rf "$DATA_DIR"
    id -u "$SERVICE_USER" >/dev/null 2>&1 && userdel "$SERVICE_USER" || true
    echo "Deleted $DATA_DIR and the $SERVICE_USER user."
else
    echo "Kept $DATA_DIR and the $SERVICE_USER user (use --purge to delete them)."
fi
