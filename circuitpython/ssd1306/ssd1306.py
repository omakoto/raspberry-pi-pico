# ssd1306/ssd1306.py
#file: font-4x5.bin
#file: font-8x16.bin
"""
SSD1306 OLED display driver and Terminal Simulator library for CircuitPython.

This library provides:
- Font: A class representing binary bitmap fonts with fast page-blitting support.
- SSD1306: A lightweight I2C driver for the SSD1306 OLED display with direct blitting.
- Term: A simple terminal simulator with cursor movement, dynamic font switching,
        scrolling, and ANSI escape sequence support.

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


def _load_font_data(filename: str) -> bytes:
    """Loads raw binary font data from the filesystem."""
    candidates: list[str] = []
    try:
        if "__file__" in globals() and __file__:
            idx = __file__.rfind("/")
            if idx != -1:
                candidates.append(__file__[:idx + 1] + filename)
    except Exception:
        pass
    candidates.extend([filename, "/" + filename])

    for path in candidates:
        try:
            with open(path, "rb") as f:
                return f.read()
        except OSError:
            pass
    raise OSError(f"Could not load font file {filename}")


class Font:
    """Represents a bitmap font for SSD1306 rendering."""

    def __init__(
        self,
        filename: str,
        glyph_width: int,
        glyph_height: int,
        cell_width: int,
        line_height: int,
        bytes_per_char: int,
        num_pages: int,
        fold_case: bool = False,
    ) -> None:
        self.filename: str = filename
        self.glyph_width: int = glyph_width
        self.glyph_height: int = glyph_height
        self.cell_width: int = cell_width
        self.line_height: int = line_height
        self.bytes_per_char: int = bytes_per_char
        self.num_pages: int = num_pages
        self.fold_case: bool = fold_case
        self.data: bytes = _load_font_data(filename)

    def get_glyph(self, c: str) -> bytes | None:
        """Returns the raw glyph bytes for a character, or None if not supported."""
        if self.fold_case:
            c = c.upper()
        code = ord(c)
        if code < 0x20 or code > 0x7E:
            return None
        offset = (code - 0x20) * self.bytes_per_char
        return self.data[offset : offset + self.bytes_per_char]


# Built-in font instances
FONT_4X5: Font = Font(
    filename="font-4x5.bin",
    glyph_width=4,
    glyph_height=5,
    cell_width=5,
    line_height=8,
    bytes_per_char=4,
    num_pages=1,
    fold_case=True,
)

FONT_8X16: Font = Font(
    filename="font-8x16.bin",
    glyph_width=8,
    glyph_height=16,
    cell_width=8,
    line_height=16,
    bytes_per_char=16,
    num_pages=2,
    fold_case=False,
)

# Backward-compatible geometry constants reflecting the default 4x5 font
FONT_WIDTH: int = FONT_4X5.glyph_width
FONT_HEIGHT: int = FONT_4X5.glyph_height
CHAR_WIDTH: int = FONT_4X5.cell_width
LINE_HEIGHT: int = FONT_4X5.line_height


def _build_font_dict(font: Font) -> dict[str, list[int]]:
    """Builds a dictionary mapping characters to lists of column bytes for backward compatibility."""
    font_dict: dict[str, list[int]] = {}
    for code in range(0x20, 0x7F):
        char = chr(code)
        if not char.islower():
            glyph = font.get_glyph(char)
            if glyph is not None:
                font_dict[char] = list(glyph[:font.glyph_width])
    return font_dict


# Backward-compatible FONT dictionary mapping
FONT: dict[str, list[int]] = _build_font_dict(FONT_4X5)


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

    def char(self, c: str, x: int, y: int, color: bool = True, font: Font | None = None) -> int:
        """Renders a single font character and returns its drawn cell width."""
        f: Font = font if font is not None else FONT_4X5
        glyph = f.get_glyph(c)
        if glyph is None:
            return 0

        # Fast page-aligned direct blit path
        if y % 8 == 0 and 0 <= x and (x + f.glyph_width) <= self.width:
            start_page: int = y // 8
            for p in range(f.num_pages):
                page: int = start_page + p
                if 0 <= page < (self.height // 8):
                    buf_idx: int = (page * self.width) + x
                    glyph_idx: int = p * f.glyph_width
                    for col in range(f.glyph_width):
                        val: int = glyph[glyph_idx + col]
                        if color:
                            self.buffer[buf_idx + col] |= val
                        else:
                            self.buffer[buf_idx + col] &= ~val
            return f.cell_width

        # Unaligned arbitrary coordinate fallback
        for p in range(f.num_pages):
            page_y: int = y + p * 8
            glyph_idx = p * f.glyph_width
            for col in range(f.glyph_width):
                val = glyph[glyph_idx + col]
                for row in range(8):
                    if val & (1 << row):
                        self.pixel(x + col, page_y + row, color)
        return f.cell_width

    def text(self, s: str, x: int, y: int, color: bool = True, font: Font | None = None) -> None:
        """Renders a string of text onto the screen buffer."""
        f: Font = font if font is not None else FONT_4X5
        curr_x: int = x
        for c in s:
            width: int = self.char(c, curr_x, y, color, font=f)
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
    """A terminal simulator on top of the SSD1306 display supporting dynamic fonts.

    Cursor coordinates are kept in pixels to match drawing primitives, allowing
    seamless switching between different font sizes.
    """

    def __init__(
        self,
        oled: SSD1306,
        font: Font | None = None,
        tab_size: int = 8,
        auto_show: bool = True,
    ) -> None:
        self.oled: SSD1306 = oled
        self.font: Font = font if font is not None else FONT_4X5
        self.tab_size: int = tab_size
        self.auto_show: bool = auto_show
        self.cursor_x: int = 0
        self.cursor_y: int = 0
        self.saved_cursor_x: int = 0
        self.saved_cursor_y: int = 0
        self._update_geometry()
        self.text_buffer: list[list[str]] = [[' '] * self.cols for _ in range(self.rows)]
        self._esc_state: int = 0  # 0: normal, 1: esc, 2: csi
        self._csi_buf: list[str] = []

    def set_font(self, font: Font) -> None:
        """Dynamically switches the active font for subsequent text output."""
        self.font = font
        self._update_geometry()

    def _update_geometry(self) -> None:
        """Updates grid dimension calculations based on active font."""
        self.cols: int = self.oled.width // self.font.cell_width
        self.rows: int = self.oled.height // self.font.line_height
        self.text_width: int = self.cols * self.font.cell_width
        self.text_height: int = self.rows * self.font.line_height

    def get_text(self) -> list[str]:
        """Returns the on-screen text, one blank-padded string per character row."""
        return ["".join(row) for row in self.text_buffer]

    def get_line(self, row: int) -> str:
        """Returns a single character row of on-screen text, blank-padded."""
        return "".join(self.text_buffer[row])

    def _cursor_col(self) -> int:
        return self.cursor_x // self.font.cell_width

    def _cursor_row(self) -> int:
        return self.cursor_y // self.font.line_height

    def _blank_text(self, row: int, col_start: int, col_end: int) -> None:
        """Blanks the text cells in [col_start, col_end) of the given row."""
        if row < 0 or row >= len(self.text_buffer):
            return
        line = self.text_buffer[row]
        for col in range(max(0, col_start), min(len(line), col_end)):
            line[col] = ' '

    def _next_line(self) -> None:
        """Moves the cursor to the start of the next line, scrolling when past the bottom."""
        self.cursor_x = 0
        self.cursor_y += self.font.line_height
        if self.cursor_y >= self.text_height:
            self.scroll_up(self.font.line_height)
            self.cursor_y = self.text_height - self.font.line_height

    def clear(self) -> None:
        """Clears the terminal screen and resets cursor position."""
        self.oled.clear()
        self.cursor_x = 0
        self.cursor_y = 0
        self.text_buffer = [[' '] * self.cols for _ in range(self.rows)]
        if self.auto_show:
            self.oled.show()

    def scroll_up(self, line_height: int | None = None) -> None:
        """Scrolls screen contents up by the given pixel height and clears the bottom area."""
        lh: int = line_height if line_height is not None else self.font.line_height
        buf = self.oled.buffer
        w = self.oled.width
        h = self.oled.height
        total_pages = h // 8
        pages = lh // 8
        if pages <= 0:
            return

        if pages < total_pages:
            buf[0 : (total_pages - pages) * w] = buf[pages * w : total_pages * w]
            for i in range((total_pages - pages) * w, len(buf)):
                buf[i] = 0x00
        else:
            for i in range(len(buf)):
                buf[i] = 0x00

        lines_to_shift = min(pages, len(self.text_buffer))
        del self.text_buffer[0:lines_to_shift]
        for _ in range(lines_to_shift):
            self.text_buffer.append([' '] * self.cols)

    def scroll_down(self, line_height: int | None = None) -> None:
        """Scrolls screen contents down by the given pixel height and clears the top area."""
        lh: int = line_height if line_height is not None else self.font.line_height
        buf = self.oled.buffer
        w = self.oled.width
        h = self.oled.height
        total_pages = h // 8
        pages = lh // 8
        if pages <= 0:
            return

        if pages < total_pages:
            buf[pages * w : total_pages * w] = buf[0 : (total_pages - pages) * w]
            for i in range(pages * w):
                buf[i] = 0x00
        else:
            for i in range(len(buf)):
                buf[i] = 0x00

        lines_to_shift = min(pages, len(self.text_buffer))
        del self.text_buffer[len(self.text_buffer) - lines_to_shift:]
        for _ in range(lines_to_shift):
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
            self.cursor_y = max(0, self.cursor_y - n * self.font.line_height)
        elif cmd == 'B':  # CUD - Cursor Down
            n = get_param(0, 1)
            self.cursor_y = min(self.text_height - self.font.line_height, self.cursor_y + n * self.font.line_height)
        elif cmd == 'C':  # CUF - Cursor Forward
            n = get_param(0, 1)
            self.cursor_x = min(self.text_width, self.cursor_x + n * self.font.cell_width)
        elif cmd == 'D':  # CUB - Cursor Back
            n = get_param(0, 1)
            self.cursor_x = max(0, self.cursor_x - n * self.font.cell_width)
        elif cmd == 'E':  # CNL - Cursor Next Line
            n = get_param(0, 1)
            self.cursor_x = 0
            self.cursor_y = min(self.text_height - self.font.line_height, self.cursor_y + n * self.font.line_height)
        elif cmd == 'F':  # CPL - Cursor Previous Line
            n = get_param(0, 1)
            self.cursor_x = 0
            self.cursor_y = max(0, self.cursor_y - n * self.font.line_height)
        elif cmd == 'G':  # CHA - Cursor Horizontal Absolute
            n = get_param(0, 1)
            self.cursor_x = max(0, min(self.text_width, (n - 1) * self.font.cell_width))
        elif cmd in ('H', 'f'):  # CUP / HVP - Cursor Position
            r = get_param(0, 1)
            c = get_param(1, 1)
            self.cursor_y = max(0, min(self.text_height - self.font.line_height, (r - 1) * self.font.line_height))
            self.cursor_x = max(0, min(self.text_width, (c - 1) * self.font.cell_width))
        elif cmd == 'J':  # ED - Erase in Display
            n = get_param(0, 0)
            row = self._cursor_row()
            col = self._cursor_col()
            if n == 0:  # Clear from cursor to end of screen
                self.oled.fill_rect(self.cursor_x, self.cursor_y, self.oled.width - self.cursor_x, self.font.line_height, False)
                self._blank_text(row, col, self.cols)
                if self.cursor_y + self.font.line_height < self.oled.height:
                    self.oled.fill_rect(0, self.cursor_y + self.font.line_height, self.oled.width, self.oled.height - (self.cursor_y + self.font.line_height), False)
                for r in range(row + 1, self.rows):
                    self._blank_text(r, 0, self.cols)
            elif n == 1:  # Clear from start of screen to cursor
                if self.cursor_y > 0:
                    self.oled.fill_rect(0, 0, self.oled.width, self.cursor_y, False)
                for r in range(0, row):
                    self._blank_text(r, 0, self.cols)
                self.oled.fill_rect(0, self.cursor_y, self.cursor_x, self.font.line_height, False)
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
                self.oled.fill_rect(self.cursor_x, self.cursor_y, self.oled.width - self.cursor_x, self.font.line_height, False)
                self._blank_text(row, col, self.cols)
            elif n == 1:  # Clear from start of line to cursor
                self.oled.fill_rect(0, self.cursor_y, self.cursor_x, self.font.line_height, False)
                self._blank_text(row, 0, col)
            elif n == 2:  # Clear entire line
                self.oled.fill_rect(0, self.cursor_y, self.oled.width, self.font.line_height, False)
                self._blank_text(row, 0, self.cols)
        elif cmd == 'S':  # SU - Scroll Up
            n = get_param(0, 1)
            for _ in range(n):
                self.scroll_up()
        elif cmd == 'T':  # SD - Scroll Down
            n = get_param(0, 1)
            for _ in range(n):
                self.scroll_down()
        elif cmd == 'm':  # SGR - Font Selection (10: default 4x5, 11: 8x16 alternate)
            for p in params:
                if p in (0, 10):
                    self.set_font(FONT_4X5)
                elif p == 11:
                    self.set_font(FONT_8X16)
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
        elif c == '\x08':  # ^H / Backspace (moves cursor back one cell without erasing)
            self.cursor_x = max(0, self.cursor_x - self.font.cell_width)
            return
        elif c == '\x09':  # ^I / Tab (Align to next tab stop)
            tab_width_px: int = self.tab_size * self.font.cell_width
            next_x: int = ((self.cursor_x // tab_width_px) + 1) * tab_width_px
            if next_x + self.font.cell_width > self.text_width:
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
        glyph = self.font.get_glyph(c)
        disp_char = c.upper() if self.font.fold_case else c

        # Wrap if the cell no longer fits on the current line
        if self.cursor_x + self.font.cell_width > self.text_width:
            self._next_line()

        # Clear the cell first so the glyph replaces whatever occupied it
        self.oled.fill_rect(self.cursor_x, self.cursor_y, self.font.cell_width, self.font.line_height, False)

        if glyph is not None:
            self.oled.char(c, self.cursor_x, self.cursor_y, True, font=self.font)

        row = self._cursor_row()
        col = self._cursor_col()
        if 0 <= row < len(self.text_buffer) and 0 <= col < len(self.text_buffer[row]):
            self.text_buffer[row][col] = disp_char if glyph is not None else ' '
        self.cursor_x += self.font.cell_width

    def get_buffer(self) -> bytearray:
        """Returns a copy of the display framebuffer."""
        return bytearray(self.oled.buffer)
