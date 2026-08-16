# ssd1306/ssd1306.py
"""
SSD1306 OLED display driver and Terminal Simulator library for CircuitPython.

This library provides:
- SSD1306: A lightweight I2C driver for the SSD1306 128x64 (or custom sizes) OLED display.
- Term: A simple terminal simulator with cursor movement, auto-wrapping, scrolling,
        and ANSI escape sequence support.
"""

try:
    import busio
except ImportError:
    # Dummy mock for host environments/testing where busio is unavailable
    class _DummyBusIO:
        class I2C:
            pass
    busio = _DummyBusIO  # type: ignore

# 3x5 Font definitions (column-major order) for rendering characters on the screen
FONT: dict[str, list[int]] = {
    ' ': [0x00, 0x00, 0x00],
    'A': [0x1E, 0x05, 0x1E],
    'B': [0x1F, 0x15, 0x0A],
    'C': [0x0E, 0x11, 0x11],
    'D': [0x1F, 0x11, 0x0E],
    'E': [0x1F, 0x15, 0x11],
    'F': [0x1F, 0x05, 0x01],
    'G': [0x0E, 0x15, 0x1D],
    'H': [0x1F, 0x04, 0x1F],
    'I': [0x11, 0x1F, 0x11],
    'J': [0x10, 0x10, 0x0F],
    'K': [0x1F, 0x04, 0x1B],
    'L': [0x1F, 0x10, 0x10],
    'M': [0x1F, 0x02, 0x0C, 0x02, 0x1F],
    'N': [0x1F, 0x02, 0x04, 0x08, 0x1F],
    'O': [0x0E, 0x11, 0x0E],
    'P': [0x1F, 0x09, 0x06],
    'Q': [0x0E, 0x11, 0x1E],
    'R': [0x1F, 0x09, 0x16],
    'S': [0x12, 0x15, 0x09],
    'T': [0x01, 0x1F, 0x01],
    'U': [0x0F, 0x10, 0x0F],
    'V': [0x07, 0x18, 0x07],
    'W': [0x1F, 0x10, 0x0E, 0x10, 0x1F],
    'X': [0x1B, 0x04, 0x1B],
    'Y': [0x03, 0x1C, 0x03],
    'Z': [0x19, 0x15, 0x13],
    '0': [0x0E, 0x11, 0x0E],
    '1': [0x12, 0x1F, 0x10],
    '2': [0x12, 0x15, 0x19],
    '3': [0x11, 0x15, 0x1F],
    '4': [0x07, 0x04, 0x1F],
    '5': [0x17, 0x15, 0x1D],
    '6': [0x1E, 0x15, 0x1D],
    '7': [0x11, 0x09, 0x07],
    '8': [0x1A, 0x15, 0x1A],
    '9': [0x13, 0x15, 0x1E],
    '-': [0x04, 0x04, 0x04],
    ':': [0x0A, 0x00, 0x00],
    '.': [0x10, 0x00, 0x00],
    '!': [0x00, 0x1D, 0x00]
}


class SSD1306:
    """A lightweight driver for the SSD1306 OLED display using pure busio.I2C."""

    def __init__(self, i2c: busio.I2C, addr: int = 0x3C, width: int = 128, height: int = 64) -> None:
        self.i2c: busio.I2C = i2c
        self.addr: int = addr
        self.width: int = width
        self.height: int = height
        self.buffer: bytearray = bytearray((width * height) // 8)

        # Standard 128x64 display initialization commands
        init_cmds: list[int] = [
            0xAE,        # Display OFF
            0xD5, 0x80,  # Set display clock divide ratio/oscillator frequency
            0xA8, 0x3F,  # Set multiplex ratio (1 to 64)
            0xD3, 0x00,  # Set display offset to 0
            0x40 | 0x00, # Set display start line to 0
            0x8D, 0x14,  # Enable charge pump
            0x20, 0x00,  # Set memory addressing mode to Horizontal
            0xA1,        # Set segment re-map (COL127 mapped to SEG0)
            0xC8,        # Set COM Output Scan Direction (remap)
            0xDA, 0x12,  # Set COM pins hardware configuration
            0x81, 0xCF,  # Set contrast control to 0xCF
            0xD9, 0xF1,  # Set pre-charge period
            0xDB, 0x40,  # Set VCOMH deselect level
            0xA4,        # Entire display ON (resume to RAM content)
            0xA6,        # Set normal display (not inverse)
            0xAF         # Display ON
        ]
        self.write_cmd(init_cmds)
        self.clear()
        self.show()

    def write_cmd(self, cmds: list[int]) -> None:
        """Sends a list of commands to the display controller."""
        self.i2c.writeto(self.addr, bytes([0x00] + cmds))

    def clear(self, color: bool = False) -> None:
        """Clears the local framebuffer buffer."""
        fill_val: int = 0xFF if color else 0x00
        for i in range(len(self.buffer)):
            self.buffer[i] = fill_val

    def pixel(self, x: int, y: int, color: bool) -> None:
        """Draws a single pixel in the framebuffer."""
        if x < 0 or x >= self.width or y < 0 or y >= self.height:
            return
        page: int = y // 8
        bit: int = y % 8
        index: int = (page * self.width) + x
        if color:
            self.buffer[index] |= (1 << bit)
        else:
            self.buffer[index] &= ~(1 << bit)

    def line(self, x0: int, y0: int, x1: int, y1: int, color: bool) -> None:
        """Draws a line from (x0, y0) to (x1, y1) using Bresenham's algorithm."""
        dx: int = abs(x1 - x0)
        dy: int = abs(y1 - y0)
        sx: int = 1 if x0 < x1 else -1
        sy: int = 1 if y0 < y1 else -1
        err: int = dx - dy
        while True:
            self.pixel(x0, y0, color)
            if x0 == x1 and y0 == y1:
                break
            e2: int = 2 * err
            if e2 > -dy:
                err -= dy
                x0 += sx
            if e2 < dx:
                err += dx
                y0 += sy

    def rect(self, x: int, y: int, w: int, h: int, color: bool) -> None:
        """Draws an unfilled rectangle outline."""
        self.line(x, y, x + w - 1, y, color)
        self.line(x, y + h - 1, x + w - 1, y + h - 1, color)
        self.line(x, y, x, y + h - 1, color)
        self.line(x + w - 1, y, x + w - 1, y + h - 1, color)

    def fill_rect(self, x: int, y: int, w: int, h: int, color: bool) -> None:
        """Draws a filled rectangle."""
        for i in range(x, x + w):
            for j in range(y, y + h):
                self.pixel(i, j, color)

    def char(self, c: str, x: int, y: int, color: bool) -> int:
        """Renders a single font character and returns its drawn width."""
        c = c.upper()
        if c not in FONT:
            return 0
        pattern: list[int] = FONT[c]
        for col_idx, col_data in enumerate(pattern):
            for row_idx in range(8):
                if col_data & (1 << row_idx):
                    self.pixel(x + col_idx, y + row_idx, color)
        return len(pattern) + 1  # Returns width of character plus 1 pixel gap

    def text(self, s: str, x: int, y: int, color: bool) -> None:
        """Renders a string of text onto the screen buffer."""
        curr_x: int = x
        for c in s:
            width: int = self.char(c, curr_x, y, color)
            curr_x += width

    def show(self) -> None:
        """Pushes the local framebuffer to the physical OLED display."""
        # Set bounds for display updates
        self.write_cmd([0x21, 0x00, 127]) # Columns 0 to 127
        self.write_cmd([0x22, 0x00, 7])   # Pages 0 to 7 (8 pixels high each)

        # Write buffer data in chunks of 128 bytes to stay within device limits
        for i in range(0, len(self.buffer), 128):
            chunk: bytearray = self.buffer[i:i+128]
            self.i2c.writeto(self.addr, bytes([0x40]) + chunk)


class Term:
    """A simple terminal simulator on top of the SSD1306 display."""

    def __init__(self, oled: SSD1306, tab_size: int = 8, auto_show: bool = True) -> None:
        self.oled: SSD1306 = oled
        self.tab_size: int = tab_size
        self.auto_show: bool = auto_show
        self.cursor_x: int = 0
        self.cursor_y: int = 0
        self.line_char_widths: list[int] = []
        self._esc_state: int = 0  # 0: normal, 1: esc, 2: csi

    def clear(self) -> None:
        """Clears the terminal screen and resets cursor position."""
        self.oled.clear()
        self.cursor_x = 0
        self.cursor_y = 0
        self.line_char_widths.clear()
        if self.auto_show:
            self.oled.show()

    def scroll_up(self) -> None:
        """Scrolls the screen contents up by one line height (8 pixels) and clears the bottom line."""
        buf = self.oled.buffer
        w = self.oled.width
        h = self.oled.height
        pages = h // 8

        # Shift all pages up by one page (8 pixels)
        buf[0 : (pages - 1) * w] = buf[w : pages * w]

        # Clear the last page (bottom 8 pixels)
        for i in range((pages - 1) * w, len(buf)):
            buf[i] = 0x00

    def print(self, s: str) -> None:
        """Prints a string to the terminal, parsing supported ANSI escape/control codes."""
        for c in s:
            self._write_char(c)
        if self.auto_show:
            self.oled.show()

    def println(self, s: str) -> None:
        """Prints a string followed by a newline."""
        self.print(s + "\n")

    def _write_char(self, c: str) -> None:
        # ESC / CSI state machine to skip unsupported ANSI escape sequences
        if self._esc_state == 1:  # ESC state
            if c == '[':
                self._esc_state = 2  # Transition to CSI state
            else:
                self._esc_state = 0  # Ignore non-CSI escape sequence for now
            return
        elif self._esc_state == 2:  # CSI state
            # CSI sequences terminate with a character in ASCII range 64 to 126 (@ to ~)
            if 64 <= ord(c) <= 126:
                self._esc_state = 0
            return

        # Control character decoding
        if c == '\x1b':  # ESC
            self._esc_state = 1
            return
        elif c == '\x07':  # ^G / Bell (Ignore)
            return
        elif c == '\x08':  # ^H / Backspace
            if self.line_char_widths:
                w = self.line_char_widths.pop()
                self.cursor_x = max(0, self.cursor_x - w)
            return
        elif c == '\x09':  # ^I / Tab (Align to next tab stop)
            tab_width_px: int = self.tab_size * 4  # Assuming standard width of 4 pixels per cell
            next_x: int = ((self.cursor_x // tab_width_px) + 1) * tab_width_px
            if next_x >= self.oled.width:
                self.cursor_x = 0
                self.cursor_y += 8
                if self.cursor_y >= self.oled.height:
                    self.scroll_up()
                    self.cursor_y = self.oled.height - 8
                self.line_char_widths.clear()
            else:
                self.line_char_widths.append(next_x - self.cursor_x)
                self.cursor_x = next_x
            return
        elif c == '\x0a':  # ^J / Line Feed
            self.cursor_x = 0
            self.cursor_y += 8
            if self.cursor_y >= self.oled.height:
                self.scroll_up()
                self.cursor_y = self.oled.height - 8
            self.line_char_widths.clear()
            return
        elif c == '\x0c':  # ^L / Form Feed (Clear Screen)
            self.clear()
            return
        elif c == '\x0d':  # ^M / Carriage Return
            self.cursor_x = 0
            self.line_char_widths.clear()
            return

        # Printable character rendering
        c_upper = c.upper()
        pattern = FONT.get(c_upper)
        # Default width to 4 if the character is not found in FONT
        width = len(pattern) + 1 if pattern is not None else 4

        # Wrap if character exceeds display width
        if self.cursor_x + width > self.oled.width:
            self.cursor_x = 0
            self.cursor_y += 8
            if self.cursor_y >= self.oled.height:
                self.scroll_up()
                self.cursor_y = self.oled.height - 8
            self.line_char_widths.clear()

        # Clear background area before drawing character (for overwrites)
        self.oled.fill_rect(self.cursor_x, self.cursor_y, width, 8, False)

        if pattern is not None:
            self.oled.char(c, self.cursor_x, self.cursor_y, True)

        self.line_char_widths.append(width)
        self.cursor_x += width
