"""Command line entry point: runs the dispatcher, or resets the web password.

Examples:
  python3 -m button_dispatcher                         # serve on port 80 (needs the capability or root)
  python3 -m button_dispatcher --port 8080 --data-dir /tmp/bd --no-ble
  python3 -m button_dispatcher --reset-password        # set a new web password
"""

from __future__ import annotations

import argparse
import getpass
import sys
from pathlib import Path
from typing import Optional

from aiohttp import web

from .app import create_app
from .auth import AuthStore, PasswordError

DEFAULT_DATA_DIR = "/var/lib/button-dispatcher"


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        prog="button-dispatcher",
        description="Turns BLE (BTHome) button presses into HTTP requests and SwitchBot actions.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__.split("Examples:")[1].join(["Examples:", ""]))
    parser.add_argument("--data-dir", default=DEFAULT_DATA_DIR,
                        help=f"where config.json and auth.json live (default {DEFAULT_DATA_DIR})")
    parser.add_argument("--host", default="0.0.0.0", help="address to listen on (default 0.0.0.0)")
    parser.add_argument("--port", type=int, default=80, help="port to listen on (default 80)")
    parser.add_argument("--no-ble", action="store_true",
                        help="don't scan for Bluetooth (the web page and Run now still work)")
    parser.add_argument("--reset-password", action="store_true",
                        help="prompt for a new web password, then exit")
    return parser.parse_args(argv)


def reset_password(data_dir: Path) -> int:
    store = AuthStore(data_dir / "auth.json")
    store.load()
    first = getpass.getpass("New password: ")
    if first != getpass.getpass("Repeat it: "):
        print("The passwords differ; nothing changed.", file=sys.stderr)
        return 1
    try:
        store.set_password(first)
    except PasswordError as e:
        print(f"{e}; nothing changed.", file=sys.stderr)
        return 1
    print("Password changed; all sessions were signed out.")
    return 0


def main(argv: Optional[list[str]] = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    data_dir = Path(args.data_dir)
    if args.reset_password:
        return reset_password(data_dir)
    web.run_app(create_app(data_dir, ble=not args.no_ble), host=args.host, port=args.port,
                print=lambda text: print(text, flush=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
