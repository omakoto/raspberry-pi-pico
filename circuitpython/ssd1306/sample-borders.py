#!/usr/bin/env circuit-run
# ssd1306/sample-borders.py
#file: ssd1306.py
#file: font-4x5.bin
#file: font-8x16.bin
"""
Sample script demonstrating all Unicode box-drawing characters (single-line,
double-line, and rounded corners) with both 4x5 and 8x16 font sizes on the SSD1306.

Advances to the next test screen automatically every 2.0 seconds, or immediately
when a push button connected to GP14 (active-low with pull-up) is pressed.

Pins used:
- SCL: GP3
- SDA: GP2
- Button: GP14 (active-low, connect between GP14 and GND)
"""

import board
import busio
import digitalio
import time
from ssd1306 import SSD1306, Term, FONT_4X5, FONT_8X16

# Pin Definitions
PIN_I2C_SCL: board.Pin = board.GP3
PIN_I2C_SDA: board.Pin = board.GP2
PIN_BUTTON: board.Pin = board.GP14

# Screen transition delay
TIMEOUT_SECONDS: float = 2.0


class ButtonReader:
    """Reads a momentary push button with debounce detection."""

    def __init__(self, pin: board.Pin) -> None:
        self.io: digitalio.DigitalInOut = digitalio.DigitalInOut(pin)
        self.io.direction = digitalio.Direction.INPUT
        self.io.pull = digitalio.Pull.UP
        self._last_state: bool = self.io.value

    def is_triggered(self) -> bool:
        """Returns True once when the button is pressed (falling edge)."""
        current_state: bool = self.io.value
        pressed: bool = (not current_state) and self._last_state
        self._last_state = current_state
        return pressed


def wait_step(btn: ButtonReader, duration_s: float = TIMEOUT_SECONDS) -> None:
    """Waits for duration_s or until the push button is pressed."""
    start = time.monotonic()
    while (time.monotonic() - start) < duration_s:
        if btn.is_triggered():
            # Wait a brief moment for debounce
            time.sleep(0.05)
            break
        time.sleep(0.01)


def test_single_line_8x16(term: Term) -> None:
    """Tests all single-line border characters with the 8x16 font."""
    term.set_font(FONT_8X16)
    term.clear()
    term.println("┌──────┬───────┐")
    term.println("│ 8x16 │ S-LINE│")
    term.println("├──────┼───────┤")
    term.print(  "└──────┴───────┘")


def test_double_line_8x16(term: Term) -> None:
    """Tests all double-line border characters with the 8x16 font."""
    term.set_font(FONT_8X16)
    term.clear()
    term.println("╔══════╦═══════╗")
    term.println("║ 8x16 ║ D-LINE║")
    term.println("╠══════╬═══════╣")
    term.print(  "╚══════╩═══════╝")


def test_rounded_corners_8x16(term: Term) -> None:
    """Tests rounded-corner border characters with the 8x16 font."""
    term.set_font(FONT_8X16)
    term.clear()
    term.println("╭──────────────╮")
    term.println("│ ROUNDED 8x16 │")
    term.println("│ ╭──────────╮ │")
    term.print(  "╰─┴──────────┴─╯")


def test_all_glyphs_table_8x16(term: Term) -> None:
    """Displays a grid of all available border glyphs in 8x16 font."""
    term.set_font(FONT_8X16)
    term.clear()
    term.println("┌─┬┐ ╔═╦╗ ╭──╮")
    term.println("│ ││ ║ ║║ │  │")
    term.println("├─┼┤ ╠═╬╣ ╰──╯")
    term.print(  "└─┴┘ ╚═╩╝ ─│═║")


def test_single_line_4x5(term: Term) -> None:
    """Tests complex single-line border grid with the 4x5 font."""
    term.set_font(FONT_4X5)
    term.clear()
    term.println("┌───────────────────────┐")
    term.println("│ SINGLE BORDER 4x5     │")
    term.println("├───────────┬───────────┤")
    term.println("│ LEFT COL  │ RIGHT COL │")
    term.println("├───────────┼───────────┤")
    term.println("│ ROW 1     │ VALUE A   │")
    term.println("│ ROW 2     │ VALUE B   │")
    term.print(  "└───────────┴───────────┘")


def test_double_line_4x5(term: Term) -> None:
    """Tests complex double-line border grid with the 4x5 font."""
    term.set_font(FONT_4X5)
    term.clear()
    term.println("╔═══════════════════════╗")
    term.println("║ DOUBLE BORDER 4x5     ║")
    term.println("╠═══════════╦═══════════╣")
    term.println("║ CHANNEL 1 ║ CHANNEL 2 ║")
    term.println("╠═══════════╬═══════════╣")
    term.println("║ DATA X    ║ DATA Y    ║")
    term.println("║ 12.34     ║ 56.78     ║")
    term.print(  "╚═══════════╩═══════════╝")


def test_rounded_corners_4x5(term: Term) -> None:
    """Tests rounded-corner dialog box layout with the 4x5 font."""
    term.set_font(FONT_4X5)
    term.clear()
    term.println("╭───────────────────────╮")
    term.println("│  SYSTEM DIALOG BOX    │")
    term.println("├───────────────────────┤")
    term.println("│ PROCEED WITH ACTION?  │")
    term.println("│                       │")
    term.println("│ ╭────────╮ ╭────────╮ │")
    term.println("│ │   OK   │ │ CANCEL │ │")
    term.print(  "╰─┴────────┴─┴────────┴─╯")


def test_all_glyphs_matrix_4x5(term: Term) -> None:
    """Displays a complete matrix of all border characters in 4x5 font."""
    term.set_font(FONT_4X5)
    term.clear()
    term.println("SINGLE: ┌─┬─┐ ├─┼─┤ └─┴─┘")
    term.println("        │ │ │ │ │ │ │ │ │")
    term.println("DOUBLE: ╔═╦═╗ ╠═╬═╣ ╚═╩═╝")
    term.println("        ║ ║ ║ ║ ║ ║ ║ ║ ║")
    term.println("ROUND:  ╭───╮ ╰───╯ ╭─┬─╮")
    term.println("TEES:   ┬ ┴ ├ ┤ ╦ ╩ ╠ ╣ ┼")
    term.println("MIX:    ┌═╦═┐ ╔─┬─╗ │ ║ ─")
    term.print(  "DONE:   PRESS GP14 / AUTO")


def run_demo() -> None:
    """Main loop cycling through all border tests."""
    print("Initializing SSD1306 on SCL=GP3, SDA=GP2, Button=GP14...")

    # Initialize I2C bus
    i2c = busio.I2C(PIN_I2C_SCL, PIN_I2C_SDA)

    # Acquire I2C lock
    while not i2c.try_lock():
        time.sleep(0.1)

    # Initialize push button
    btn = ButtonReader(PIN_BUTTON)

    try:
        # Initialize SSD1306 display and terminal
        oled = SSD1306(i2c, addr=0x3C, width=128, height=64)
        term = Term(oled)

        tests = [
            ("8x16 Single-Line", test_single_line_8x16),
            ("8x16 Double-Line", test_double_line_8x16),
            ("8x16 Rounded Corners", test_rounded_corners_8x16),
            ("8x16 Glyphs Table", test_all_glyphs_table_8x16),
            ("4x5 Single-Line", test_single_line_4x5),
            ("4x5 Double-Line", test_double_line_4x5),
            ("4x5 Rounded Corners", test_rounded_corners_4x5),
            ("4x5 Glyphs Matrix", test_all_glyphs_matrix_4x5),
        ]

        while True:
            for name, test_fn in tests:
                print(f"Running: {name}...")
                test_fn(term)
                wait_step(btn, duration_s=TIMEOUT_SECONDS)

    finally:
        i2c.unlock()


if __name__ == "__main__":
    run_demo()
