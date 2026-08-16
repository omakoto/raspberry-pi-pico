#!/usr/bin/env circuit-run
# ssd1306/sample.py
#file: ssd1306.py
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


def run_demo() -> None:
    print("Initializing SSD1306 display on SCL=GP3, SDA=GP2...")

    # Initialize I2C bus
    i2c = busio.I2C(board.GP3, board.GP2)

    # Wait to acquire I2C lock
    while not i2c.try_lock():
        time.sleep(0.1)

    try:
        # Initialize SSD1306 display
        oled = SSD1306(i2c, addr=0x3C, width=128, height=64)

        # Initialize Terminal Simulator
        term = Term(oled)

        # 1. Basic print and println
        term.println("OLED TERMINAL DEMO")
        term.print("Initializing...\n")
        time.sleep(1.0)

        # 2. Tabs demonstration (^I)
        term.println("TESTING TABS:")
        term.print("1\t2\t3\n")
        time.sleep(1.0)

        # 3. Backspace demonstration (^H)
        term.print("Erase this: ERROR\b\b\b\b\bOK   \n")
        time.sleep(1.0)

        # 4. Auto-wrapping demonstration
        term.println("TESTING WRAPPING:")
        term.println("THIS IS A VERY LONG STRING THAT WILL AUTOMATICALLY WRAP TO THE NEXT LINE ONCE IT EXCEEDS THE SCREEN WIDTH")
        time.sleep(2.0)

        # 5. Scrolling demonstration
        term.println("TESTING SCROLLING:")
        for i in range(1, 10):
            term.println(f"Log line number {i}")
            time.sleep(0.5)

        term.println("DEMO DONE!")

    finally:
        i2c.unlock()


if __name__ == "__main__":
    run_demo()
