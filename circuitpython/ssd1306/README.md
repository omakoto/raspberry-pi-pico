# SSD1306 OLED Driver & Terminal Simulator

A lightweight, high-performance CircuitPython library for 128×64 I2C SSD1306 OLED displays.

This library provides:
- [`SSD1306`](file:///usr/local/google/home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/ssd1306/ssd1306.py#L221): Direct 2D framebuffer graphics driver (pixels, lines, rectangles, character/string blitting).
- [`Term`](file:///usr/local/google/home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/ssd1306/ssd1306.py#L365): Terminal simulator featuring cursor movement, scrolling, tab stops, dynamic font switching, and ANSI CSI escape sequence decoding.
- [`Font`](file:///usr/local/google/home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/ssd1306/ssd1306.py#L48): Bitmap font loader with fast page-aligned blitting. Includes built-in compact [`FONT_4X5`](file:///usr/local/google/home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/ssd1306/ssd1306.py#L84) and full-height [`FONT_8X16`](file:///usr/local/google/home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/ssd1306/ssd1306.py#L95) fonts.
- **Procedural Unicode Box Drawing**: Full support for single-line, double-line, and rounded corner box characters that seamlessly tile across text cells.

---

## Table of Contents

- [Hardware & Wiring](#hardware--wiring)
- [Quick Start](#quick-start)
  - [Terminal Simulator Mode (`Term`)](#terminal-simulator-mode-term)
  - [Direct Graphics Mode (`SSD1306`)](#direct-graphics-mode-ssd1306)
- [API Reference](#api-reference)
  - [`SSD1306`](#class-ssd1306)
  - [`Term`](#class-term)
  - [`Font`](#class-font)
- [Supported Characters & Fonts](#supported-characters--fonts)
  - [Font Specifications](#font-specifications)
  - [7-Bit Printable ASCII](#7-bit-printable-ascii)
  - [Unicode Box-Drawing Characters](#unicode-box-drawing-characters)
- [Control Characters & ANSI Escape Sequences](#control-characters--ansi-escape-sequences)
  - [C0 Control Characters](#c0-control-characters)
  - [ANSI CSI Escape Sequences](#ansi-csi-escape-sequences)
- [Sample Scripts](#sample-scripts)
- [Running Unit Tests](#running-unit-tests)

---

## Hardware & Wiring

Default I2C pin mapping on Raspberry Pi Pico:

| SSD1306 Pin | Pico Pin | Description |
| :--- | :--- | :--- |
| **SCL** | **GP3** (Pin 5) | I2C0 SCL Clock line |
| **SDA** | **GP2** (Pin 4) | I2C0 SDA Data line |
| **VCC** / **VDD** | **3V3(OUT)** (Pin 36) | 3.3V Power Supply |
| **GND** | **GND** (Pin 38 or Pin 3) | Ground |

*Default I2C 7-bit Address:* `0x3C` (some modules use `0x3D`).

---

## Quick Start

### Terminal Simulator Mode (`Term`)

The [`Term`](file:///usr/local/google/home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/ssd1306/ssd1306.py#L365) class provides a high-level terminal emulator that handles cursor tracking, autowrap, scrolling, and ANSI escape sequences.

```python
import board
import busio
import time
from ssd1306 import SSD1306, Term, FONT_4X5, FONT_8X16

# 1. Initialize I2C bus and acquire lock
i2c = busio.I2C(board.GP3, board.GP2)
while not i2c.try_lock():
    time.sleep(0.1)

# 2. Instantiate display driver and terminal simulator
oled = SSD1306(i2c, addr=0x3C, width=128, height=64)
term = Term(oled)

# 3. Print text and ANSI escape sequences
term.println("HELLO WORLD!")
term.println("Line 2: Tab\tSeparated")
term.print("\x1b[11m8x16 FONT\x1b[10m 4x5 font\n")

# 4. Box drawing with Unicode
term.println("┌─────────────┐")
term.println("│ STATUS: OK  │")
term.print(  "└─────────────┘")
```

### Direct Graphics Mode (`SSD1306`)

For drawing shapes, charts, or custom layouts, use the [`SSD1306`](file:///usr/local/google/home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/ssd1306/ssd1306.py#L221) driver directly.

```python
import board
import busio
import time
from ssd1306 import SSD1306, FONT_4X5, FONT_8X16

i2c = busio.I2C(board.GP3, board.GP2)
while not i2c.try_lock():
    time.sleep(0.1)

oled = SSD1306(i2c, addr=0x3C, width=128, height=64)

# Clear display buffer
oled.clear()

# Draw geometry primitives
oled.rect(0, 0, 128, 64, True)         # Outer frame border
oled.line(0, 20, 127, 20, True)        # Header divider line
oled.fill_rect(10, 28, 40, 12, True)   # Solid rectangle

# Draw text at exact pixel coordinates
oled.text("DASHBOARD", 10, 3, True, font=FONT_8X16)
oled.text("Sensor: Active", 10, 48, True, font=FONT_4X5)

# Push local framebuffer to physical OLED panel
oled.show()
```

---

## API Reference

### Class `SSD1306`

Low-level driver managing the monochrome 1-bit framebuffer buffer and I2C commands.

| Method / Property | Description |
| :--- | :--- |
| `SSD1306(i2c, addr=0x3C, width=128, height=64)` | Initializes the display hardware and allocates a `(width * height) // 8` byte buffer. |
| `clear(color=False)` | Clears the local buffer (`False` = black/off, `True` = white/on). |
| `pixel(x, y, color)` | Draws or clears a single pixel at `(x, y)`. |
| `line(x0, y0, x1, y1, color)` | Draws a line from `(x0, y0)` to `(x1, y1)` using Bresenham's algorithm. |
| `rect(x, y, w, h, color)` | Draws an unfilled rectangle outline. |
| `fill_rect(x, y, w, h, color)` | Draws a filled rectangle. |
| `char(c, x, y, color=True, font=None) -> int` | Renders a single character `c` at pixel coordinates `(x, y)`. Returns drawn cell width. |
| `text(s, x, y, color=True, font=None)` | Renders string `s` starting at pixel coordinates `(x, y)`. |
| `show()` | Transmits dirty framebuffer memory to the physical SSD1306 controller via I2C. |
| `write_cmd(cmds)` | Sends raw command byte list to the display controller. |

### Class `Term`

Terminal emulator wrapping an `SSD1306` instance. Maintains a cursor, shadow text grid, and escape parser.

| Method / Property | Description |
| :--- | :--- |
| `Term(oled, font=None, tab_size=8, auto_show=True, autowrap=True)` | Creates terminal simulator. Defaults to [`FONT_4X5`](file:///usr/local/google/home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/ssd1306/ssd1306.py#L84). |
| `print(s)` | Prints string `s`, parsing control characters and ANSI CSI escape sequences. Flushes if `auto_show=True`. |
| `println(s)` | Prints string `s` followed by `\n`. |
| `clear()` | Clears the terminal display and resets the cursor to `(0, 0)`. |
| `set_font(font)` | Dynamically switches active font for subsequent output, updating line height and grid calculations. |
| `scroll_up(line_height=None)` | Scrolls framebuffer and text buffer up by `line_height` pixels (defaults to active font line height). |
| `scroll_down(line_height=None)` | Scrolls framebuffer and text buffer down by `line_height` pixels. |
| `get_text() -> list[str]` | Returns a snapshot list of strings representing visible characters on each terminal row. |
| `get_line(row: int) -> str` | Returns blank-padded string content for character row `row`. |
| `get_buffer() -> bytearray` | Returns a copy of the raw display framebuffer. |

### Class `Font`

Represents binary bitmap fonts stored in raw binary files.

| Attribute / Method | Description |
| :--- | :--- |
| `Font(filename, glyph_width, glyph_height, cell_width, line_height, bytes_per_char, num_pages, fold_case=False)` | Loads font bitmap data from `.bin` file on disk. |
| `get_glyph(c: str) -> bytes \| None` | Returns raw column/page bytes for character `c`, or `None` if unmapped. |

---

## Supported Characters & Fonts

### Font Specifications

The library ships with two pre-compiled binary fonts:

| Font | Glyph (W×H) | Cell (W×H) | Display Grid (128×64) | Case Support | Font File |
| :--- | :--- | :--- | :--- | :--- | :--- |
| [`FONT_4X5`](file:///usr/local/google/home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/ssd1306/ssd1306.py#L84) *(Default)* | 4×5 px | 5×8 px | **25 columns × 8 rows** (200 chars) | Folded (lowercase auto-folded to uppercase) | [`font-4x5.bin`](file:///usr/local/google/home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/ssd1306/font-4x5.bin) |
| [`FONT_8X16`](file:///usr/local/google/home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/ssd1306/ssd1306.py#L95) | 8×16 px | 8×16 px | **16 columns × 4 rows** (64 chars) | Full (distinct uppercase & lowercase) | [`font-8x16.bin`](file:///usr/local/google/home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/ssd1306/font-8x16.bin) |

### 7-Bit Printable ASCII

All 95 printable 7-bit ASCII characters from code point `0x20` (Space) to `0x7E` (`~` Tilde) are supported:

```text
  ! " # $ % & ' ( ) * + , - . /
0 1 2 3 4 5 6 7 8 9 : ; < = > ?
@ A B C D E F G H I J K L M N O
P Q R S T U V W X Y Z [ \ ] ^ _
` a b c d e f g h i j k l m n o
p q r s t u v w x y z { | } ~
```

- In `FONT_4X5`, lowercase characters (`a`–`z`) are automatically rendered as uppercase glyphs (`A`–`Z`).
- In `FONT_8X16`, lowercase characters have unique, fully proportioned lowercase glyphs with ascenders and descenders.
- Any character outside ASCII `0x20`–`0x7E` that is not a recognized Unicode box character renders as an empty blank cell.

### Unicode Box-Drawing Characters

The library includes built-in procedural rendering for 26 Unicode box-drawing characters. These characters span the entire cell width and line height, connecting smoothly across adjacent characters and rows without gaps.

#### 1. Single-Line Borders (11 Characters)

| Character | Code Point | Unicode Name | Description |
| :---: | :--- | :--- | :--- |
| `─` | `U+2500` | Box Drawings Light Horizontal | Horizontal straight line |
| `│` | `U+2502` | Box Drawings Light Vertical | Vertical straight line |
| `┌` | `U+250C` | Box Drawings Light Down and Right | Top-left corner |
| `┐` | `U+2510` | Box Drawings Light Down and Left | Top-right corner |
| `└` | `U+2514` | Box Drawings Light Up and Right | Bottom-left corner |
| `┘` | `U+2518` | Box Drawings Light Up and Left | Bottom-right corner |
| `├` | `U+251C` | Box Drawings Light Vertical and Right | Left T-junction |
| `┤` | `U+2524` | Box Drawings Light Vertical and Left | Right T-junction |
| `┬` | `U+252C` | Box Drawings Light Down and Horizontal | Top T-junction |
| `┴` | `U+2534` | Box Drawings Light Up and Horizontal | Bottom T-junction |
| `┼` | `U+253C` | Box Drawings Light Vertical and Horizontal | 4-way cross junction |

#### 2. Double-Line Borders (11 Characters)

| Character | Code Point | Unicode Name | Description |
| :---: | :--- | :--- | :--- |
| `═` | `U+2550` | Box Drawings Double Horizontal | Double horizontal straight line |
| `║` | `U+2551` | Box Drawings Double Vertical | Double vertical straight line |
| `╔` | `U+2554` | Box Drawings Double Down and Right | Double top-left corner |
| `╗` | `U+2557` | Box Drawings Double Down and Left | Double top-right corner |
| `╚` | `U+255A` | Box Drawings Double Up and Right | Double bottom-left corner |
| `╝` | `U+255D` | Box Drawings Double Up and Left | Double bottom-right corner |
| `╠` | `U+2560` | Box Drawings Double Vertical and Right | Double left T-junction |
| `╣` | `U+2563` | Box Drawings Double Vertical and Left | Double right T-junction |
| `╦` | `U+2566` | Box Drawings Double Down and Horizontal | Double top T-junction |
| `╩` | `U+2569` | Box Drawings Double Up and Horizontal | Double bottom T-junction |
| `╬` | `U+256C` | Box Drawings Double Vertical and Horizontal | Double 4-way cross junction |

#### 3. Rounded Corners (4 Characters)

| Character | Code Point | Unicode Name | Description |
| :---: | :--- | :--- | :--- |
| `╭` | `U+256D` | Box Drawings Light Arc Down and Right | Rounded top-left corner |
| `╮` | `U+256E` | Box Drawings Light Arc Down and Left | Rounded top-right corner |
| `╯` | `U+256F` | Box Drawings Light Arc Up and Left | Rounded bottom-right corner |
| `╰` | `U+2570` | Box Drawings Light Arc Up and Right | Rounded bottom-left corner |

#### Box Drawing Examples

```text
Single Line:         Double Line:         Rounded Box:
┌──────┬──────┐     ╔══════╦══════╗     ╭──────────────╮
│ TEMP │ 24.5 │     ║ SENS ║ DATA ║     │ SYSTEM MENU  │
├──────┼──────┤     ╠══════╬══════╣     ├──────────────┤
│ HUM  │ 48.2 │     ║ CH 1 ║  OK  ║     │ 1. SETTINGS  │
└──────┴──────┘     ╚══════╩══════╝     ╰──────────────╯
```

---

## Control Characters & ANSI Escape Sequences

The [`Term`](file:///usr/local/google/home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/ssd1306/ssd1306.py#L365) class implements a state machine that interprets C0 control codes and ANSI CSI (Control Sequence Introducer) sequences.

### C0 Control Characters

| Escape / Code | Name | Effect |
| :--- | :--- | :--- |
| `\x1b` | **ESC** | Initiates an ANSI escape sequence state. |
| `\x07` (`\a`) | **BEL** | Bell character. Silently ignored. |
| `\x08` (`\b`) | **BS** | Backspace. Moves cursor left by 1 character cell without erasing. Clamped at column 0. |
| `\x09` (`\t`) | **TAB** | Horizontal Tab. Advances cursor to next tab stop (multiple of `tab_size`, default 8). Wraps if past right margin. |
| `\x0a` (`\n`) | **LF** | Line Feed. Moves cursor to column 0 of next line. Automatically scrolls display if on bottom row. |
| `\x0c` (`\f`) | **FF** | Form Feed. Clears entire screen and resets cursor position to `(0, 0)`. |
| `\x0d` (`\r`) | **CR** | Carriage Return. Returns cursor to column 0 of current line without advancing vertically. |

### ANSI CSI Escape Sequences

All CSI sequences begin with `\x1b[` (or `\033[`). Parameters are semicolon-separated decimal numbers (omitted parameters default to 1 or command default).

#### 1. Cursor Movement

| Sequence | Name | Description |
| :--- | :--- | :--- |
| `\x1b[<n>A` | **CUU** | Cursor Up by `n` lines (clamped at top row 0). |
| `\x1b[<n>B` | **CUD** | Cursor Down by `n` lines (clamped at bottom row). |
| `\x1b[<n>C` | **CUF** | Cursor Forward / Right by `n` columns (clamped at right margin). |
| `\x1b[<n>D` | **CUB** | Cursor Back / Left by `n` columns (clamped at left margin 0). |
| `\x1b[<n>E` | **CNL** | Cursor Next Line: moves down `n` lines and homes to column 0. |
| `\x1b[<n>F` | **CPL** | Cursor Previous Line: moves up `n` lines and homes to column 0. |
| `\x1b[<n>G` | **CHA** | Cursor Horizontal Absolute: moves cursor to column `n` (1-indexed). |
| `\x1b[<r>;<c>H`<br>`\x1b[<r>;<c>f` | **CUP**<br>**HVP** | Cursor Position: moves cursor to row `r`, column `c` (1-indexed; defaults to `1;1`). |
| `\x1b[s` | **SCOSC** | Save Cursor Position. |
| `\x1b[u` | **SCORC** | Restore Cursor Position. |

#### 2. Erasing & Clearing

| Sequence | Name | Description |
| :--- | :--- | :--- |
| `\x1b[0J` or `\x1b[J` | **ED 0** | Erase in Display: Clears from cursor position to end of screen. |
| `\x1b[1J` | **ED 1** | Erase in Display: Clears from start of screen to cursor position. |
| `\x1b[2J` or `\x1b[3J` | **ED 2/3** | Erase in Display: Clears entire display screen (cursor remains unchanged). |
| `\x1b[0K` or `\x1b[K` | **EL 0** | Erase in Line: Clears from cursor position to end of line. |
| `\x1b[1K` | **EL 1** | Erase in Line: Clears from start of line to cursor position. |
| `\x1b[2K` | **EL 2** | Erase in Line: Clears entire current line. |

#### 3. Scrolling

| Sequence | Name | Description |
| :--- | :--- | :--- |
| `\x1b[<n>S` | **SU** | Scroll Up display contents by `n` text lines (default 1). |
| `\x1b[<n>T` | **SD** | Scroll Down display contents by `n` text lines (default 1). |

#### 4. Dynamic Font Selection (SGR)

| Sequence | Description |
| :--- | :--- |
| `\x1b[0m` or `\x1b[10m` | Selects primary default font: [`FONT_4X5`](file:///usr/local/google/home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/ssd1306/ssd1306.py#L84) (5×8 cell). |
| `\x1b[11m` | Selects alternate large font: [`FONT_8X16`](file:///usr/local/google/home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/ssd1306/ssd1306.py#L95) (8×16 cell). |

*Example:*
```python
term.print("\x1b[11mLARGE HEADER\x1b[10m\nSmall body text\n")
```

#### 5. Private Modes (Auto-wrap Control)

| Sequence | Mode | Description |
| :--- | :--- | :--- |
| `\x1b[?7h` | **DECAWM Set** | Enables automatic line wrapping at right margin (default). |
| `\x1b[?7l` | **DECAWM Reset** | Disables auto-wrapping. Characters written past right edge overwrite last column cell. |

---

## Sample Scripts

Several executable sample scripts demonstrating various features are provided in this directory:

| Script | Description |
| :--- | :--- |
| [`sample.py`](file:///usr/local/google/home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/ssd1306/sample.py) | Comprehensive demo: basic print/println, tab stops, backspace overwriting, line wrapping, vertical scrolling, ANSI cursor/erase sequences, and ASCII table. |
| [`sample-large.py`](file:///usr/local/google/home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/ssd1306/sample-large.py) | Demonstrates 8×16 font rendering, dynamic font switching (`Term.set_font` & ANSI `\x1b[11m`), mixed font layouts, and direct framebuffer graphics. |
| [`sample-borders.py`](file:///usr/local/google/home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/ssd1306/sample-borders.py) | Showcases all 26 Unicode single-line, double-line, and rounded box-drawing characters across both 4×5 and 8×16 font sizes. Supports button stepping via GP14. |
| [`sample-ascii.py`](file:///usr/local/google/home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/ssd1306/sample-ascii.py) | Standalone ASCII table demo displaying all printable characters formatted by high nibble. |

### Running on Hardware

Using [`circuit-run`](file:///usr/local/google/home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/bin/circuit-run):

```bash
# Execute sample directly
./sample.py

# Or run other demos
./sample-large.py
./sample-borders.py
./sample-ascii.py
```

---

## Running Unit Tests

The test suite in [`test_term.py`](file:///usr/local/google/home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/ssd1306/test_term.py) runs on host CPython without requiring physical hardware:

```bash
python3 test_term.py
```

The test runner:
- Validates font binary properties and coverage of all printable ASCII characters.
- Tests control character handling (`\b`, `\t`, `\n`, `\r`, `\f`, `\a`).
- Tests scrolling, clipping, autowrapping, and edge-boundary conditions.
- Exercises all supported ANSI CSI escape sequences.
- Verifies that the shadow text buffer and pixel framebuffer match bit-for-bit after every operation.
