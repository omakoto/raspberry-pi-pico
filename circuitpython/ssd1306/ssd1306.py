# ssd1306/ssd1306.py
#file: font-4x5.bin
"""
SSD1306 OLED display driver and Terminal Simulator library for CircuitPython.

This library provides:
- SSD1306: A lightweight I2C driver for the SSD1306 128x64 (or custom sizes) OLED display.
- Term: A simple terminal simulator with cursor movement, auto-wrapping, scrolling,
        and ANSI escape sequence support.

Run test_term.py in this directory every time this file is touched:
    python3 test_term.py
"""

try:
    import busio
except ImportError:
    # Dummy mock for host environments/testing where busio is unavailable
    class _DummyBusIO:
        class I2C:
            pass
    busio = _DummyBusIO  # type: ignore

# Cell geometry. Every glyph is exactly FONT_WIDTH x FONT_HEIGHT pixels so the terminal is
# a uniform character grid: CHAR_WIDTH leaves a one pixel gap between neighbouring cells,
# and LINE_HEIGHT matches the display's 8 pixel page height so scrolling is a page copy
# (it also leaves 3 blank rows below each glyph as line spacing).
FONT_WIDTH: int = 4
FONT_HEIGHT: int = 5
CHAR_WIDTH: int = 5
LINE_HEIGHT: int = 8


def _load_font(filename: str = "font-4x5.bin") -> dict[str, list[int]]:
    """Loads 4x5 font definitions from the external binary file.

    The binary file contains 4-byte column slices for ASCII characters 0x20..0x7E
    in column-major order with bit 0 as the top row.
    """
    candidates: list[str] = []
    try:
        if "__file__" in globals() and __file__:
            idx = __file__.rfind("/")
            if idx != -1:
                candidates.append(__file__[:idx + 1] + filename)
    except Exception:
        pass
    candidates.extend([filename, "/" + filename])

    data: bytes | None = None
    for path in candidates:
        try:
            with open(path, "rb") as f:
                data = f.read()
                break
        except OSError:
            pass
    if data is None:
        raise OSError(f"Could not load font file {filename}")

    font: dict[str, list[int]] = {}
    for code in range(0x20, 0x7F):
        char = chr(code)
        if not char.islower():
            offset = (code - 0x20) * 4
            font[char] = list(data[offset:offset + 4])
    return font


# 4x5 Font definitions (column-major order with bit 0 as top row).
# Every printable ASCII character (0x20..0x7E) has a glyph. Lower case letters
# are deliberately absent so rendering converts to upper case first.
FONT: dict[str, list[int]] = _load_font()



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
        return CHAR_WIDTH  # Glyph width plus the 1 pixel inter-character gap

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
    """A simple terminal simulator on top of the SSD1306 display.

    Because every glyph occupies a uniform CHAR_WIDTH x LINE_HEIGHT cell, the terminal
    is a plain character grid. Alongside the pixel framebuffer, Term keeps a shadow text
    buffer holding the character visible in each cell, so callers can read the screen
    contents back as text (see get_text()) without decoding pixels.

    Cursor coordinates are kept in pixels to match the drawing primitives; they are
    always cell-aligned, so the grid indices are simply cursor_x // CHAR_WIDTH and
    cursor_y // LINE_HEIGHT.
    """

    def __init__(self, oled: SSD1306, tab_size: int = 8, auto_show: bool = True) -> None:
        self.oled: SSD1306 = oled
        self.tab_size: int = tab_size
        self.auto_show: bool = auto_show
        self.cursor_x: int = 0
        self.cursor_y: int = 0
        self.saved_cursor_x: int = 0
        self.saved_cursor_y: int = 0
        self.cols: int = oled.width // CHAR_WIDTH
        self.rows: int = oled.height // LINE_HEIGHT
        # A display is not necessarily an exact number of cells wide or tall (a 128 pixel
        # wide panel holds 25 five pixel cells and 3 spare pixels). Confine the cursor to
        # the whole cells so it always stays grid aligned; the remainder is never drawn on.
        self.text_width: int = self.cols * CHAR_WIDTH
        self.text_height: int = self.rows * LINE_HEIGHT
        self.text_buffer: list[list[str]] = [[' '] * self.cols for _ in range(self.rows)]
        self._esc_state: int = 0  # 0: normal, 1: esc, 2: csi
        self._csi_buf: list[str] = []

    def get_text(self) -> list[str]:
        """Returns the on-screen text, one blank-padded string per character row."""
        return ["".join(row) for row in self.text_buffer]

    def get_line(self, row: int) -> str:
        """Returns a single character row of on-screen text, blank-padded."""
        return "".join(self.text_buffer[row])

    def _cursor_col(self) -> int:
        return self.cursor_x // CHAR_WIDTH

    def _cursor_row(self) -> int:
        return self.cursor_y // LINE_HEIGHT

    def _blank_text(self, row: int, col_start: int, col_end: int) -> None:
        """Blanks the text cells in [col_start, col_end) of the given row."""
        if row < 0 or row >= self.rows:
            return
        line = self.text_buffer[row]
        for col in range(max(0, col_start), min(self.cols, col_end)):
            line[col] = ' '

    def _next_line(self) -> None:
        """Moves the cursor to the start of the next line, scrolling when past the bottom."""
        self.cursor_x = 0
        self.cursor_y += LINE_HEIGHT
        if self.cursor_y >= self.text_height:
            self.scroll_up()
            self.cursor_y = self.text_height - LINE_HEIGHT

    def clear(self) -> None:
        """Clears the terminal screen and resets cursor position."""
        self.oled.clear()
        self.cursor_x = 0
        self.cursor_y = 0
        for row in range(self.rows):
            self._blank_text(row, 0, self.cols)
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

        del self.text_buffer[0]
        self.text_buffer.append([' '] * self.cols)

    def scroll_down(self) -> None:
        """Scrolls the screen contents down by one line height (8 pixels) and clears the top line."""
        buf = self.oled.buffer
        w = self.oled.width
        h = self.oled.height
        pages = h // 8

        # Shift all pages down by one page (8 pixels)
        buf[w : pages * w] = buf[0 : (pages - 1) * w]

        # Clear the first page (top 8 pixels)
        for i in range(w):
            buf[i] = 0x00

        del self.text_buffer[-1]
        self.text_buffer.insert(0, [' '] * self.cols)

    def print(self, s: str) -> None:
        """Prints a string to the terminal, parsing supported ANSI escape/control codes."""
        for c in s:
            self._write_char(c)
        if self.auto_show:
            self.oled.show()

    def println(self, s: str) -> None:
        """Prints a string followed by a newline."""
        self.print(s + "\n")

    def _execute_csi(self, cmd: str) -> None:
        # Parse parameters
        param_str = "".join(self._csi_buf)
        parts = param_str.split(";")
        params: list[int] = []
        for p in parts:
            if p:
                try:
                    params.append(int(p))
                except ValueError:
                    params.append(0)
            else:
                params.append(0)

        def get_param(index: int, default: int) -> int:
            if index < len(params) and params[index] != 0:
                return params[index]
            return default

        # Execute commands
        if cmd == 'A':  # CUU - Cursor Up
            n = get_param(0, 1)
            self.cursor_y = max(0, self.cursor_y - n * LINE_HEIGHT)
        elif cmd == 'B':  # CUD - Cursor Down
            n = get_param(0, 1)
            self.cursor_y = min(self.text_height - LINE_HEIGHT, self.cursor_y + n * LINE_HEIGHT)
        elif cmd == 'C':  # CUF - Cursor Forward
            n = get_param(0, 1)
            self.cursor_x = min(self.text_width, self.cursor_x + n * CHAR_WIDTH)
        elif cmd == 'D':  # CUB - Cursor Back
            n = get_param(0, 1)
            self.cursor_x = max(0, self.cursor_x - n * CHAR_WIDTH)
        elif cmd == 'E':  # CNL - Cursor Next Line
            n = get_param(0, 1)
            self.cursor_x = 0
            self.cursor_y = min(self.text_height - LINE_HEIGHT, self.cursor_y + n * LINE_HEIGHT)
        elif cmd == 'F':  # CPL - Cursor Previous Line
            n = get_param(0, 1)
            self.cursor_x = 0
            self.cursor_y = max(0, self.cursor_y - n * LINE_HEIGHT)
        elif cmd == 'G':  # CHA - Cursor Horizontal Absolute
            n = get_param(0, 1)
            self.cursor_x = max(0, min(self.text_width, (n - 1) * CHAR_WIDTH))
        elif cmd in ('H', 'f'):  # CUP / HVP - Cursor Position
            r = get_param(0, 1)
            c = get_param(1, 1)
            self.cursor_y = max(0, min(self.text_height - LINE_HEIGHT, (r - 1) * LINE_HEIGHT))
            self.cursor_x = max(0, min(self.text_width, (c - 1) * CHAR_WIDTH))
        elif cmd == 'J':  # ED - Erase in Display
            n = get_param(0, 0)
            row = self._cursor_row()
            col = self._cursor_col()
            if n == 0:  # Clear from cursor to end of screen
                self.oled.fill_rect(self.cursor_x, self.cursor_y, self.oled.width - self.cursor_x, LINE_HEIGHT, False)
                self._blank_text(row, col, self.cols)
                if self.cursor_y + LINE_HEIGHT < self.oled.height:
                    self.oled.fill_rect(0, self.cursor_y + LINE_HEIGHT, self.oled.width, self.oled.height - (self.cursor_y + LINE_HEIGHT), False)
                for r in range(row + 1, self.rows):
                    self._blank_text(r, 0, self.cols)
            elif n == 1:  # Clear from start of screen to cursor
                if self.cursor_y > 0:
                    self.oled.fill_rect(0, 0, self.oled.width, self.cursor_y, False)
                for r in range(0, row):
                    self._blank_text(r, 0, self.cols)
                self.oled.fill_rect(0, self.cursor_y, self.cursor_x, LINE_HEIGHT, False)
                self._blank_text(row, 0, col)
            elif n in (2, 3):  # Clear entire screen
                self.oled.clear()
                for r in range(self.rows):
                    self._blank_text(r, 0, self.cols)
        elif cmd == 'K':  # EL - Erase in Line
            n = get_param(0, 0)
            row = self._cursor_row()
            col = self._cursor_col()
            if n == 0:  # Clear from cursor to end of line
                self.oled.fill_rect(self.cursor_x, self.cursor_y, self.oled.width - self.cursor_x, LINE_HEIGHT, False)
                self._blank_text(row, col, self.cols)
            elif n == 1:  # Clear from start of line to cursor
                self.oled.fill_rect(0, self.cursor_y, self.cursor_x, LINE_HEIGHT, False)
                self._blank_text(row, 0, col)
            elif n == 2:  # Clear entire line
                self.oled.fill_rect(0, self.cursor_y, self.oled.width, LINE_HEIGHT, False)
                self._blank_text(row, 0, self.cols)
        elif cmd == 'S':  # SU - Scroll Up
            n = get_param(0, 1)
            for _ in range(n):
                self.scroll_up()
        elif cmd == 'T':  # SD - Scroll Down
            n = get_param(0, 1)
            for _ in range(n):
                self.scroll_down()
        elif cmd == 's':  # Save cursor position
            self.saved_cursor_x = self.cursor_x
            self.saved_cursor_y = self.cursor_y
        elif cmd == 'u':  # Restore cursor position
            self.cursor_x = self.saved_cursor_x
            self.cursor_y = self.saved_cursor_y

    def _write_char(self, c: str) -> None:
        # ESC / CSI state machine to handle ANSI escape sequences
        if self._esc_state == 1:  # ESC state
            if c == '[':
                self._esc_state = 2  # Transition to CSI state
                self._csi_buf.clear()
            else:
                self._esc_state = 0  # Ignore non-CSI escape sequence for now
            return
        elif self._esc_state == 2:  # CSI state
            if 48 <= ord(c) <= 63:
                self._csi_buf.append(c)
            elif 64 <= ord(c) <= 126:
                self._execute_csi(c)
                self._esc_state = 0
                self._csi_buf.clear()
            return

        # Control character decoding
        if c == '\x1b':  # ESC
            self._esc_state = 1
            return
        elif c == '\x07':  # ^G / Bell (Ignore)
            return
        elif c == '\x08':  # ^H / Backspace (moves the cursor back one cell without erasing)
            self.cursor_x = max(0, self.cursor_x - CHAR_WIDTH)
            return
        elif c == '\x09':  # ^I / Tab (Align to next tab stop)
            tab_width_px: int = self.tab_size * CHAR_WIDTH
            next_x: int = ((self.cursor_x // tab_width_px) + 1) * tab_width_px
            if next_x >= self.text_width:
                self._next_line()
            else:
                self.cursor_x = next_x
            return
        elif c == '\x0a':  # ^J / Line Feed
            self._next_line()
            return
        elif c == '\x0c':  # ^L / Form Feed (Clear Screen)
            self.clear()
            return
        elif c == '\x0d':  # ^M / Carriage Return
            self.cursor_x = 0
            return

        # Printable character rendering
        c_upper = c.upper()
        pattern = FONT.get(c_upper)

        # Wrap if the cell no longer fits on the current line
        if self.cursor_x + CHAR_WIDTH > self.text_width:
            self._next_line()

        # Clear the cell first so the glyph replaces whatever occupied it
        self.oled.fill_rect(self.cursor_x, self.cursor_y, CHAR_WIDTH, LINE_HEIGHT, False)

        if pattern is not None:
            self.oled.char(c_upper, self.cursor_x, self.cursor_y, True)

        # The shadow buffer mirrors what is visible: the font is upper case only, and a
        # character with no glyph leaves the cell blank.
        self.text_buffer[self._cursor_row()][self._cursor_col()] = c_upper if pattern is not None else ' '
        self.cursor_x += CHAR_WIDTH

    def get_buffer(self) -> bytearray:
        """Returns a copy of the display framebuffer."""
        return bytearray(self.oled.buffer)
