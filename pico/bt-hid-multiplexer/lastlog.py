#!/usr/bin/env python3
"""Reads the previous run's log from the bt-hid-multiplexer over its VIAL RawHID interface.

After the firmware's watchdog has rebooted a hung board, the end of the previous run's log and a
trail of its last events survive in RAM. This prints them without needing the USB serial port
(which is only present when GP10 is grounded). It needs access to the hidraw node, see
~/cbin/setup/config-hidraw-permission.

Usage:
    ./lastlog.py          # summary plus the previous log
    ./lastlog.py --info   # summary only

Run it before unplugging the board: a power cycle clears what survived.
"""

import argparse
import glob
import os
import select
import sys
from typing import List, Optional

DEBUG_PREFIX = 0xFD
CRUMB_SOURCES = {0x1: "HCI", 0x2: "SM", 0x3: "GATT", 0x4: "classic", 0x5: "flash", 0x6: "pairing"}


def find_raw_hid() -> Optional[str]:
    """Returns the hidraw node of the multiplexer's VIAL interface (usage page 0xFF60), if any."""
    for d in sorted(glob.glob("/sys/class/hidraw/hidraw*")):
        try:
            if "0003:00002E8A:0000000C" not in open(d + "/device/uevent").read():
                continue
            if open(d + "/device/report_descriptor", "rb").read().startswith(bytes([0x06, 0x60, 0xFF])):
                return "/dev/" + os.path.basename(d)
        except OSError:
            continue
    return None


def request(fd: int, payload: List[int]) -> bytes:
    """Sends one 32-byte request and returns the 32-byte reply."""
    os.write(fd, bytes([0] + payload) + bytes(32 - len(payload)))
    if not select.select([fd], [], [], 2.0)[0]:
        raise TimeoutError("no reply from the board (is it hung? unplug and re-plug it)")
    return os.read(fd, 64)


def main() -> int:
    parser = argparse.ArgumentParser(description="Read the previous run's log from the multiplexer.")
    parser.add_argument("--info", action="store_true", help="print only the summary")
    args = parser.parse_args()

    node = find_raw_hid()
    if node is None:
        print("The multiplexer's VIAL interface was not found (is it plugged in?).", file=sys.stderr)
        return 1
    fd = os.open(node, os.O_RDWR | os.O_NONBLOCK)

    info = request(fd, [DEBUG_PREFIX, 0x00])
    valid, watchdog = info[2], info[3]
    length = int.from_bytes(info[4:8], "little")
    uptime_ms = int.from_bytes(info[8:12], "little")
    stage = int.from_bytes(info[12:14], "little")
    crumb_count = info[14]
    crumbs = [int.from_bytes(info[15 + 2 * i:17 + 2 * i], "little") for i in range(crumb_count)]

    if not valid:
        print("No previous log: the board was powered on rather than reset, so nothing survived.")
        return 0
    print(f"Previous run {'ended in a WATCHDOG RESET' if watchdog else 'ended'} after {uptime_ms} ms; "
          f"main loop was at stage {stage}.")
    print("Last events, oldest first: " + " ".join(
        f"{c:04X}({CRUMB_SOURCES.get(c >> 12, '?')})" for c in crumbs))
    if args.info:
        return 0

    data = bytearray()
    while len(data) < length:
        reply = request(fd, [DEBUG_PREFIX, 0x01, len(data) & 0xFF, len(data) >> 8])
        count = reply[2]
        if count == 0:
            break
        data += reply[8:8 + count]
    print(f"---- previous run's log (last {len(data)} bytes) ----")
    sys.stdout.write(data.decode("latin1").replace("\r", ""))
    print("\n---- end ----")
    return 0


if __name__ == "__main__":
    sys.exit(main())
