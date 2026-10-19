#!/bin/bash
# install.sh: install or upgrade button-dispatcher on this Raspberry Pi (Raspberry Pi OS / Debian).
#
# Run it on the Pi, as root or through sudo. Most of the time you'll use ./deploy.sh from your PC,
# which copies this directory to the Pi and runs this script there.
#
# What it does: installs the few apt packages needed, creates a `button-dispatcher` service user,
# copies the program to /opt/button-dispatcher, builds a Python virtualenv there, installs the
# systemd service and starts it. Running it again upgrades in place; the data in
# /var/lib/button-dispatcher (config, password) is kept.
#
# Options:
#   -p, --port PORT   port for the web page (default 80)
#   -h, --help        show this help
#
# Examples:
#   sudo ./install.sh
#   sudo ./install.sh --port 8080
#
# Uninstall with ./uninstall.sh.

set -euo pipefail

APP_DIR=/opt/button-dispatcher
DATA_DIR=/var/lib/button-dispatcher
SERVICE_USER=button-dispatcher
UNIT=/etc/systemd/system/button-dispatcher.service
WRAPPER=/usr/local/bin/button-dispatcher
SRC_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

usage() {
    sed -n '2,/^$/{s/^# \{0,1\}//;p}' "${BASH_SOURCE[0]}"
}

die() {
    echo "install.sh: $*" >&2
    exit 1
}

port=80
opts=$(getopt -o p:h --long port:,help -n install.sh -- "$@") || exit 2
eval set -- "$opts"
while true; do
    case "$1" in
        -p|--port) port=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        --) shift; break ;;
        *) die "internal error parsing options" ;;
    esac
done
[[ $# -eq 0 ]] || die "unexpected argument: $1 (see --help)"
[[ $port =~ ^[0-9]+$ && $port -ge 1 && $port -le 65535 ]] || die "invalid port: $port"

if [[ $EUID -ne 0 ]]; then
    exec sudo "$0" --port "$port"
fi

command -v apt-get >/dev/null || die "this installer needs a Debian-based system (apt-get not found)"
command -v systemctl >/dev/null || die "this installer needs systemd"
[[ -d $SRC_DIR/button_dispatcher ]] || die "run it from the project directory (button_dispatcher/ not found next to it)"

echo "==> Installing system packages"
packages=(python3 python3-venv bluez ca-certificates curl)
missing=()
for p in "${packages[@]}"; do
    dpkg -s "$p" >/dev/null 2>&1 || missing+=("$p")
done
if [[ ${#missing[@]} -gt 0 ]]; then
    apt-get update -qq
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq "${missing[@]}"
fi

python3 -c 'import sys; sys.exit(0 if sys.version_info >= (3, 11) else 1)' ||
    die "Python 3.11 or newer is required (found $(python3 --version))"

echo "==> Creating the service user and directories"
if ! id -u "$SERVICE_USER" >/dev/null 2>&1; then
    useradd --system --home-dir "$DATA_DIR" --no-create-home --shell /usr/sbin/nologin \
        --groups bluetooth "$SERVICE_USER"
else
    usermod -aG bluetooth "$SERVICE_USER"
fi
install -d -m 0755 "$APP_DIR"
install -d -m 0700 -o "$SERVICE_USER" -g "$SERVICE_USER" "$DATA_DIR"

echo "==> Copying the program to $APP_DIR"
# Replace the package wholesale so files deleted from the project don't linger; tests aren't needed.
rm -rf "$APP_DIR/button_dispatcher"
tar -C "$SRC_DIR" --exclude='*_test.py' --exclude='__pycache__' -cf - button_dispatcher |
    tar -C "$APP_DIR" -xf -
install -m 0644 "$SRC_DIR/requirements.txt" "$SRC_DIR/README.md" "$APP_DIR/"
chown -R root:root "$APP_DIR/button_dispatcher"

echo "==> Setting up the Python environment"
[[ -x $APP_DIR/venv/bin/python ]] || python3 -m venv "$APP_DIR/venv"
"$APP_DIR/venv/bin/pip" install --quiet --disable-pip-version-check -r "$APP_DIR/requirements.txt"

echo "==> Installing the service (port $port)"
sed "s/--port 80\$/--port $port/" "$SRC_DIR/button-dispatcher.service" >"$UNIT"
chmod 0644 "$UNIT"
cat >"$WRAPPER" <<'EOF'
#!/bin/bash
# Runs the button-dispatcher command line (for example --reset-password) as the service user.
if [[ $EUID -ne 0 ]]; then
    exec sudo "$0" "$@"
fi
# The program is not installed into the virtualenv; python finds it in the current directory.
cd /opt/button-dispatcher
exec runuser -u button-dispatcher -- /opt/button-dispatcher/venv/bin/python -m button_dispatcher \
    --data-dir /var/lib/button-dispatcher "$@"
EOF
chmod 0755 "$WRAPPER"
systemctl daemon-reload
systemctl enable button-dispatcher >/dev/null 2>&1
systemctl restart button-dispatcher

echo "==> Waiting for the web page"
ok=0
for _ in $(seq 1 30); do
    if curl -fsS -o /dev/null "http://127.0.0.1:$port/api/auth/status" 2>/dev/null; then
        ok=1
        break
    fi
    sleep 1
done
if [[ $ok -ne 1 ]]; then
    systemctl --no-pager status button-dispatcher || true
    journalctl -u button-dispatcher -n 30 --no-pager || true
    die "the service did not start; see the log above"
fi

suffix=""
[[ $port -eq 80 ]] || suffix=":$port"
echo
echo "button-dispatcher is running."
echo "  Open:   http://$(hostname).local$suffix/"
for ip in $(hostname -I 2>/dev/null); do
    echo "          http://$ip$suffix/"
done
echo "  Set the password right away: until then, anyone on your network can."
echo "  Logs:   journalctl -u button-dispatcher -f"
echo "  Forgot the password: button-dispatcher --reset-password"
