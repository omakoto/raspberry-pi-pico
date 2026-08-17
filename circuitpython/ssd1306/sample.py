#!/usr/bin/env circuit-run
# ssd1306/sample.py
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

        # 1. Basic print and println
        print("Step 1: Printing text...")
        term.println("OLED TERMINAL DEMO")
        term.print("Initializing...\n")
        time.sleep(1.0)

        # 2. Tabs demonstration (^I)
        print("Step 2: Testing tabs...")
        term.println("TESTING TABS:")
        term.print("1\t2\t3\n")
        time.sleep(1.0)

        # 3. Backspace demonstration (^H)
        print("Step 3: Testing backspaces...")
        term.print("Erase this: ERROR\b\b\b\b\bOK   \n")
        time.sleep(1.0)

        # 4. Auto-wrapping demonstration
        print("Step 4: Testing auto-wrapping...")
        term.println("TESTING WRAPPING:")
        term.println("THIS IS A VERY LONG STRING THAT WILL AUTOMATICALLY WRAP TO THE NEXT LINE ONCE IT EXCEEDS THE SCREEN WIDTH")
        time.sleep(2.0)

        # 5. Scrolling demonstration
        print("Step 5: Testing scrolling...")
        term.println("TESTING SCROLLING:")
        for i in range(1, 6):
            term.println(f"Log line number {i}")
            time.sleep(0.3)

        # 6. CSI Escape Sequence Demonstration
        print("Step 6: Testing CSI escape sequences...")
        term.println("TESTING ANSI CSI:")
        time.sleep(1.0)

        # Save cursor, move to (0,0), write OVERWRITE, then restore cursor and write RESTORED!
        print("Sub-step 6a: Save and restore cursor...")
        term.print("\x1b[s")      # Save cursor position
        term.print("\x1b[1;1H")   # Move cursor to home (row 1, col 1)
        term.print("OVERWRITE")
        time.sleep(1.0)

        term.print("\x1b[u")      # Restore cursor position
        term.println("RESTORED!")
        time.sleep(1.0)

        # Cursor movement: Move up 2 lines, print UP HERE!, then move down 2 lines
        print("Sub-step 6b: Cursor up/down movement...")
        term.print("\x1b[2A")      # Move up 2 lines
        term.print("UP HERE!")
        time.sleep(1.0)
        term.print("\x1b[2B")      # Move down 2 lines

        # Erase line: print, move cursor back, clear line from cursor
        print("Sub-step 6c: Erasing line...")
        term.print("TO BE ERASED...")
        time.sleep(1.0)
        term.print("\x1b[14D")     # Move cursor back over the 14 characters just printed
        term.print("\x1b[K")       # Clear line from cursor to end of line
        term.println("ERASED OK!")
        time.sleep(1.5)

        # 7. Full printable ASCII character set
        print("Step 7: Showing the ASCII table...")
        show_ascii_table(term)
        time.sleep(5.0)

        term.print("\f")
        term.println("DEMO DONE!")
        print("Demo completed successfully!")

    finally:
        i2c.unlock()


if __name__ == "__main__":
    run_demo()
