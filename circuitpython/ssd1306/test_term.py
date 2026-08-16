# ssd1306/test_term.py
"""
Unit tests for the SSD1306 Term simulator library.
This script verifies the cursor movement, word wrapping, scrolling, backspace, tab, 
carriage return, and ANSI escape sequence parsing logic using a mock I2C display.
"""

from ssd1306 import SSD1306, Term

class MockI2C:
    """Mock I2C bus that ignores physical write commands."""

    def writeto(self, addr: int, data: bytes) -> None:
        pass


def test_basic_print() -> None:
    i2c = MockI2C()
    # Cast MockI2C to busio.I2C for typing compatibility
    import typing
    oled = SSD1306(typing.cast(typing.Any, i2c), width=128, height=64)
    term = Term(oled)

    # 1. Print hello world
    term.print("HELLO")
    assert term.cursor_x == 5 * 4  # Each character is 3px wide + 1px gap = 4px
    assert term.cursor_y == 0

    # 2. Print newline
    term.print("\n")
    assert term.cursor_x == 0
    assert term.cursor_y == 8

    # 3. Print tab
    term.print("\t")
    # Tab stop is every 8 cells / 32 pixels. Starts at 0, so should align to 32
    assert term.cursor_x == 32
    assert term.cursor_y == 8

    # 4. Backspace
    term.print("\b")
    # Should move cursor back by tab width (32px) since the last "char" printed was a tab
    assert term.cursor_x == 0
    assert term.cursor_y == 8

    # Let's write 'A' (4px) and backspace
    term.print("A")
    assert term.cursor_x == 4
    term.print("\b")
    assert term.cursor_x == 0

    # 5. Carriage return
    term.print("ABC")
    assert term.cursor_x == 12
    term.print("\r")
    assert term.cursor_x == 0

    # 6. Auto-wrapping
    # With width=128, a line can fit 128 // 4 = 32 characters of width 4.
    # Let's print 33 characters
    term.print("A" * 33)
    assert term.cursor_x == 4  # Wrapped to next line and printed the 33rd char
    assert term.cursor_y == 16 # y=8 + 8 = 16

    # 7. Scrolling
    # height=64, so 8 rows (y = 0, 8, 16, 24, 32, 40, 48, 56)
    # Cursor is currently at y=16. Let's move it to the bottom line (y=56)
    term.print("\n" * 5)  # Now at y = 56 (row 7)
    assert term.cursor_y == 56

    # Printing one more newline should trigger scroll, cursor remains at y=56
    term.print("\n")
    assert term.cursor_y == 56

    # 8. Clear screen
    term.print("\f")
    assert term.cursor_x == 0
    assert term.cursor_y == 0

    # 9. ANSI escape sequence ignoring
    # \x1b[31m (red) and \x1b[?25h (show cursor) should be ignored and not advance cursor
    term.print("\x1b[31mABC\x1b[?25h")
    # Only "ABC" should be rendered
    assert term.cursor_x == 12
    assert term.cursor_y == 0

    print("All Term unit tests passed successfully!")


def test_csi_sequences() -> None:
    i2c = MockI2C()
    import typing
    oled = SSD1306(typing.cast(typing.Any, i2c), width=128, height=64)
    term = Term(oled)

    # Move cursor down by 2 lines (CUD: CSI 2 B)
    term.print("\x1b[2B")
    assert term.cursor_y == 16
    assert term.cursor_x == 0

    # Move cursor up by 1 line (CUU: CSI A or CSI 1 A)
    term.print("\x1b[A")
    assert term.cursor_y == 8

    # Move cursor forward by 10 columns (CUF: CSI 10 C)
    term.print("\x1b[10C")
    assert term.cursor_x == 40

    # Move cursor back by 5 columns (CUB: CSI 5 D)
    term.print("\x1b[5D")
    assert term.cursor_x == 20

    # Cursor position: row 5, col 8 (CUP: CSI 5 ; 8 H)
    term.print("\x1b[5;8H")
    assert term.cursor_y == 32
    assert term.cursor_x == 28

    # Save cursor position (CSI s)
    term.print("\x1b[s")

    # Move cursor to home (CSI H or CSI f)
    term.print("\x1b[H")
    assert term.cursor_x == 0
    assert term.cursor_y == 0

    # Restore cursor position (CSI u)
    term.print("\x1b[u")
    assert term.cursor_x == 28
    assert term.cursor_y == 32

    print("All CSI unit tests passed successfully!")


if __name__ == "__main__":
    test_basic_print()
    test_csi_sequences()
