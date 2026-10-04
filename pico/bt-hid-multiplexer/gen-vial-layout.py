#!/usr/bin/env python3
"""Generates src/vial_layout.h, the VIAL keyboard definition served to the web configurator.

The matrix is 16x16 and a key's matrix position is its virtual key (row * 16 + col), which for
keyboard keys is the HID usage (see "Virtual Matrix configuration" in src/config.h). The mouse
buttons and motion directions are virtual keys 0xE8-0xEF and 0xF0-0xF7.

Usage:
    ./gen-vial-layout.py            # rewrites src/vial_layout.h
    ./gen-vial-layout.py --print    # prints the uncompressed JSON instead

Run it after changing the key list below, then rebuild the firmware.
"""

import argparse
import json
import lzma
import os
from typing import Dict, List, Tuple

# (virtual key, x, y, width, height) in key units.
Key = Tuple[int, float, float, float, float]


def build_keys() -> List[Key]:
    keys: List[Key] = []

    def add(vkey: int, x: float, y: float, w: float = 1, h: float = 1) -> None:
        keys.append((vkey, x, y, w, h))

    def run(vkeys: List[int], x: float, y: float) -> float:
        for vkey in vkeys:
            add(vkey, x, y)
            x += 1
        return x

    # Function row.
    add(0x29, 0, 0)  # Esc
    run([0x3A, 0x3B, 0x3C, 0x3D], 2, 0)
    run([0x3E, 0x3F, 0x40, 0x41], 6.5, 0)
    run([0x42, 0x43, 0x44, 0x45], 11, 0)
    run([0x46, 0x47, 0x48], 15.25, 0)  # PrtSc, ScrLk, Pause

    # Number row.
    run([0x35] + list(range(0x1E, 0x28)) + [0x2D, 0x2E], 0, 1.5)
    add(0x2A, 13, 1.5, 2)  # Backspace
    run([0x49, 0x4A, 0x4B], 15.25, 1.5)  # Ins, Home, PgUp
    run([0x53, 0x54, 0x55, 0x56], 18.5, 1.5)  # NumLock / * -

    # QWERTY row.
    add(0x2B, 0, 2.5, 1.5)  # Tab
    run([0x14, 0x1A, 0x08, 0x15, 0x17, 0x1C, 0x18, 0x0C, 0x12, 0x13, 0x2F, 0x30], 1.5, 2.5)
    add(0x31, 13.5, 2.5, 1.5)  # Backslash
    run([0x4C, 0x4D, 0x4E], 15.25, 2.5)  # Del, End, PgDn
    run([0x5F, 0x60, 0x61], 18.5, 2.5)  # Keypad 7 8 9
    add(0x57, 21.5, 2.5, 1, 2)  # Keypad +

    # Home row.
    add(0x39, 0, 3.5, 1.75)  # Caps Lock
    run([0x04, 0x16, 0x07, 0x09, 0x0A, 0x0B, 0x0D, 0x0E, 0x0F, 0x33, 0x34], 1.75, 3.5)
    add(0x28, 12.75, 3.5, 2.25)  # Enter
    run([0x5C, 0x5D, 0x5E], 18.5, 3.5)  # Keypad 4 5 6

    # Bottom letter row.
    add(0xE1, 0, 4.5, 2.25)  # Left Shift
    run([0x1D, 0x1B, 0x06, 0x19, 0x05, 0x11, 0x10, 0x36, 0x37, 0x38], 2.25, 4.5)
    add(0xE5, 12.25, 4.5, 2.75)  # Right Shift
    add(0x52, 16.25, 4.5)  # Up
    run([0x59, 0x5A, 0x5B], 18.5, 4.5)  # Keypad 1 2 3
    add(0x58, 21.5, 4.5, 1, 2)  # Keypad Enter

    # Space row.
    add(0xE0, 0, 5.5, 1.25)  # Left Ctrl
    add(0xE3, 1.25, 5.5, 1.25)  # Left GUI
    add(0xE2, 2.5, 5.5, 1.25)  # Left Alt
    add(0x2C, 3.75, 5.5, 6.25)  # Space
    add(0xE6, 10, 5.5, 1.25)  # Right Alt
    add(0xE7, 11.25, 5.5, 1.25)  # Right GUI
    add(0x65, 12.5, 5.5, 1.25)  # Menu
    add(0xE4, 13.75, 5.5, 1.25)  # Right Ctrl
    run([0x50, 0x51, 0x4F], 15.25, 5.5)  # Left, Down, Right
    add(0x62, 18.5, 5.5, 2)  # Keypad 0
    add(0x63, 20.5, 5.5)  # Keypad .

    # ISO / Japanese keys, Hangul/Kana.
    run([0x64, 0x32, 0x87, 0x88, 0x89, 0x8A, 0x8B, 0x90, 0x91], 0, 7)
    # F13-F24.
    run(list(range(0x68, 0x74)), 0, 8.25)

    # Mouse: buttons 1-8, then cursor up/down/left/right and wheel up/down/left/right.
    run(list(range(0xE8, 0xF0)), 0, 9.75)
    run(list(range(0xF0, 0xF8)), 9, 9.75)
    return keys


def to_kle(keys: List[Key]) -> List[list]:
    """Converts absolute key rectangles to Keyboard Layout Editor rows."""
    rows_by_y: Dict[float, List[Key]] = {}
    for key in keys:
        rows_by_y.setdefault(key[2], []).append(key)

    kle: List[list] = []
    kle_y = 0.0  # Where KLE puts the next row without a "y" property.
    for y in sorted(rows_by_y):
        row: list = []
        x = 0.0
        for vkey, kx, _, w, h in sorted(rows_by_y[y], key=lambda k: k[1]):
            props: Dict[str, float] = {}
            if not row and y != kle_y:
                props["y"] = y - kle_y
            if kx != x:
                props["x"] = kx - x
            if w != 1:
                props["w"] = w
            if h != 1:
                props["h"] = h
            if props:
                row.append(props)
            row.append(f"{vkey // 16},{vkey % 16}")
            x = kx + w
        kle.append(row)
        kle_y = y + 1
    return kle


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate src/vial_layout.h.")
    parser.add_argument("--print", action="store_true", help="print the JSON instead of writing the header")
    args = parser.parse_args()

    definition = {
        "name": "Pico W BLE HID Multiplexer",
        "vendorId": "0x2E8A",
        "productId": "0x000C",
        "lighting": "none",
        "matrix": {"rows": 16, "cols": 16},
        "layouts": {"keymap": to_kle(build_keys())},
    }
    text = json.dumps(definition, separators=(",", ":"))
    if args.print:
        print(text)
        return

    data = lzma.compress(text.encode(), format=lzma.FORMAT_XZ)
    lines = []
    for i in range(0, len(data), 16):
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in data[i:i + 16]) + ",")
    body = "\n".join(lines)
    header = f"""#ifndef VIAL_LAYOUT_H_
#define VIAL_LAYOUT_H_

#include <stdint.h>
#include <stddef.h>

#ifndef PROGMEM
#define PROGMEM
#endif

// Generated by gen-vial-layout.py; do not edit by hand.
static const uint8_t VIAL_KEYBOARD_DEF[] PROGMEM = {{
{body}
}};

static const size_t VIAL_KEYBOARD_DEF_SIZE = {len(data)};

#endif // VIAL_LAYOUT_H_
"""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "src", "vial_layout.h")
    with open(path, "w") as f:
        f.write(header)
    print(f"Wrote {path}: {len(text)} bytes of JSON, {len(data)} compressed")


if __name__ == "__main__":
    main()
