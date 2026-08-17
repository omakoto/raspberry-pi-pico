#!/usr/bin/env circuit-run
# ssd1306/sample-ascii.py
#file: ssd1306.py
#file: font-4x5.bin
#file: font-8x16.bin
"""
Sample script demonstrating the Term class terminal simulator on SSD1306 OLED display.
Pins used:
- SCL: GP3
- SDA: GP2
"""

import board
import busio
import time
from ssd1306 import SSD1306, Term

# Pin Definitions
PIN_I2C_SCL: board.Pin = board.GP3
PIN_I2C_SDA: board.Pin = board.GP2


def show_ascii_table(term: Term) -> None:
    """Draws every printable ASCII character as a table of 16 columns per high nibble.

    A 128 pixel wide screen holds 25 characters, so a "4:" style row label plus 16
    characters fits, and the six nibble rows plus a heading fill the 8 available lines
    exactly. Lower case has no glyphs of its own, so the last two rows echo the two
    upper case rows above them.
    """
    term.print("\f")  # Form feed: clear the screen and home the cursor
    term.println("ASCII TABLE:")
    for high in range(0x20, 0x80, 0x10):
        chars = "".join(chr(c) for c in range(high, min(high + 0x10, 0x7F)))
        term.println("%X:%s" % (high >> 4, chars))


def run_demo() -> None:
    print("Initializing SSD1306 display on SCL=GP3, SDA=GP2...")

    # Initialize I2C bus
    i2c = busio.I2C(PIN_I2C_SCL, PIN_I2C_SDA)

    # Wait to acquire I2C lock
    while not i2c.try_lock():
        time.sleep(0.1)

    try:
        # Initialize SSD1306 display
        oled = SSD1306(i2c, addr=0x3C, width=128, height=64)

        # Initialize Terminal Simulator
        term = Term(oled)
        show_ascii_table(term)

    finally:
        i2c.unlock()


if __name__ == "__main__":
    run_demo()
