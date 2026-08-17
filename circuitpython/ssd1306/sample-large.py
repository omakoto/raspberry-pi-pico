#!/usr/bin/env circuit-run
# ssd1306/sample-large.py
#file: ssd1306.py
#file: font-4x5.bin
#file: font-8x16.bin
"""
Sample script demonstrating large font (8x16) and mixed font (4x5 & 8x16)
rendering on the SSD1306 OLED display using the ssd1306 library.

Features demonstrated:
1. Large 8x16 font display (16 cols x 4 rows) with upper/lowercase glyphs.
2. Dynamic font switching via Term.set_font().
3. Inline ANSI SGR font switching escape sequences (\\x1b[11m and \\x1b[10m).
4. Mixed font layout combining large headers and compact detail lines.

Pins used:
- SCL: GP3
- SDA: GP2
"""

import board
import busio
import time
from ssd1306 import SSD1306, Term, FONT_4X5, FONT_8X16

# Pin Definitions
PIN_I2C_SCL: board.Pin = board.GP3
PIN_I2C_SDA: board.Pin = board.GP2


def demo_large_font(term: Term) -> None:
    """Demonstrates terminal output using the 8x16 large font."""
    term.set_font(FONT_8X16)
    term.clear()

    term.println("LARGE FONT 8x16")
    term.println("16 Cols x 4 Rows")
    term.println("Upper & lower!")
    term.println("0123456789 +-=")
    time.sleep(3.0)


def demo_mixed_fonts_terminal(term: Term) -> None:
    """Demonstrates dynamic font switching in Term to mix headers and body text."""
    term.clear()

    # Large header line using set_font
    term.set_font(FONT_8X16)
    term.println("DEVICE STATUS")

    # Switch to compact 4x5 font for detailed info
    term.set_font(FONT_4X5)
    term.println("CPU: RP2040 Dual Core @ 133MHz")
    term.println("RAM: 264 KB SRAM")
    term.println("FLASH: 2 MB Quad-SPI")
    term.println("I2C: 400kHz (GP2/GP3)")
    term.println("STATUS: RUNNING NORMAL")
    time.sleep(3.0)


def demo_ansi_font_switching(term: Term) -> None:
    """Demonstrates inline font switching using standard ANSI SGR escape sequences."""
    term.clear()

    # \x1b[11m selects alternate 8x16 font, \x1b[10m (or \x1b[0m) restores default 4x5 font
    term.print("\x1b[11mSENSOR DATA\x1b[10m\n")
    term.print("-------------------------\n")
    term.print("\x1b[11m24.5C\x1b[10m Temp (Ambient)\n")
    term.print("\x1b[11m48.2%\x1b[10m Humidity (RH)\n")
    term.print("-------------------------\n")
    time.sleep(3.0)


def demo_direct_mixed_drawing(oled: SSD1306) -> None:
    """Demonstrates direct framebuffer drawing with mixed fonts and border frame."""
    oled.clear()

    # Draw border frame
    oled.rect(0, 0, 128, 64, True)
    oled.line(0, 20, 127, 20, True)

    # Large title in header box
    oled.text("CircuitPython", 12, 3, True, font=FONT_8X16)

    # Small details in body box
    oled.text("SSD1306 OLED Driver", 6, 25, True, font=FONT_4X5)
    oled.text("Font 8x16: 16x4 chars", 6, 35, True, font=FONT_4X5)
    oled.text("Font 4x5:  25x8 chars", 6, 45, True, font=FONT_4X5)
    oled.text(">> Dynamic Blitting <<", 6, 54, True, font=FONT_4X5)

    oled.show()
    time.sleep(3.0)


def run_demo() -> None:
    """Initializes hardware and cycles through large and mixed font demonstrations."""
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

        while True:
            print("Step 1: Demonstrating large 8x16 font...")
            demo_large_font(term)

            print("Step 2: Demonstrating mixed fonts via set_font()...")
            demo_mixed_fonts_terminal(term)

            print("Step 3: Demonstrating inline ANSI SGR font switching...")
            demo_ansi_font_switching(term)

            print("Step 4: Demonstrating direct mixed drawing with borders...")
            demo_direct_mixed_drawing(oled)

            term.set_font(FONT_8X16)
            term.clear()
            term.println("LOOP RESTARTING")
            time.sleep(1.5)

    finally:
        i2c.unlock()


if __name__ == "__main__":
    run_demo()
