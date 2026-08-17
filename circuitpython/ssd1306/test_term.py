#!/usr/bin/env python3
# ssd1306/test_term.py
"""
Unit tests for the SSD1306 Term terminal simulator.

Runs on CPython against a mock I2C bus - no hardware required:
    python3 test_term.py

The tests assert on Term.get_text(), the shadow text buffer holding the character
visible in every cell of the terminal grid. Because that buffer is bookkeeping kept
alongside the pixel framebuffer, most tests also call assert_consistent(), which
re-renders the text buffer from scratch and requires it to reproduce the framebuffer
bit for bit - so the two can never silently drift apart.

Geometry the expectations below are built on: cells are CHAR_WIDTH x LINE_HEIGHT = 5x8
pixels, so a 128x64 display is a 25x8 character grid, and the 40x24 displays used by the
scrolling tests are a more tractable 8x3.
"""

import sys
import typing

from ssd1306 import CHAR_WIDTH, FONT, FONT_HEIGHT, FONT_WIDTH, LINE_HEIGHT, SSD1306, Term


# A character outside printable ASCII, so the font deliberately has no glyph for it.
NO_GLYPH = 'é'  # e-acute


class MockI2C:
    """Mock I2C bus that records writes instead of talking to hardware."""

    def __init__(self) -> None:
        self.writes: int = 0

    def writeto(self, addr: int, data: bytes) -> None:
        self.writes += 1


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def make_term(width: int = 128, height: int = 64, **kwargs: typing.Any) -> Term:
    """Creates a Term backed by a mock display. Defaults to a 25x8 character grid."""
    oled = SSD1306(typing.cast(typing.Any, MockI2C()), width=width, height=height)
    return Term(oled, **kwargs)


def dump_pixels(oled: SSD1306) -> list[str]:
    """Renders the framebuffer as one '#'/'.' string per pixel row, for failure output."""
    rows = []
    for y in range(oled.height):
        page = y // 8
        bit = 1 << (y % 8)
        rows.append("".join('#' if oled.buffer[page * oled.width + x] & bit else '.'
                            for x in range(oled.width)))
    return rows


def fail(label: str, expected: list[str], actual: list[str]) -> None:
    lines = ["%s\n  %-10s | %s" % (label, "expected", "actual")]
    for i in range(max(len(expected), len(actual))):
        e = expected[i] if i < len(expected) else "<missing>"
        a = actual[i] if i < len(actual) else "<missing>"
        mark = "  " if e == a else "->"
        lines.append("%s%d: %r | %r" % (mark, i, e, a))
    raise AssertionError("\n".join(lines))


def assert_screen(term: Term, expected: list[str], label: str = "screen") -> None:
    """Asserts the on-screen text. Trailing blanks and omitted trailing rows are ignored."""
    actual = [line.rstrip() for line in term.get_text()]
    want = [line.rstrip() for line in expected]
    want += [""] * (term.rows - len(want))
    if actual != want:
        fail(label, want, actual)
    assert_consistent(term, label)


def assert_cursor(term: Term, x: int, y: int, label: str = "cursor") -> None:
    assert (term.cursor_x, term.cursor_y) == (x, y), \
        "%s: expected (%d, %d), got (%d, %d)" % (label, x, y, term.cursor_x, term.cursor_y)


def assert_consistent(term: Term, label: str = "consistency") -> None:
    """Requires the text buffer to be an exact description of the pixel framebuffer.

    Re-draws every non-blank cell onto a blank display at its grid position; because the
    font is fixed width, that reconstruction must match the real framebuffer exactly.
    Any stale pixel Term forgot to clear, or any cell it forgot to record, shows up here.
    """
    ref = SSD1306(typing.cast(typing.Any, MockI2C()), width=term.oled.width, height=term.oled.height)
    for row, line in enumerate(term.get_text()):
        for col, char in enumerate(line):
            if char != ' ':
                ref.char(char, col * CHAR_WIDTH, row * LINE_HEIGHT, True)
    if ref.buffer != term.oled.buffer:
        fail("%s: text buffer does not describe the framebuffer" % label,
             dump_pixels(ref), dump_pixels(term.oled))


TESTS: list[typing.Callable[[], None]] = []


def test(fn: typing.Callable[[], None]) -> typing.Callable[[], None]:
    TESTS.append(fn)
    return fn


# ---------------------------------------------------------------------------
# Font / geometry
# ---------------------------------------------------------------------------

@test
def test_font_is_fixed_width() -> None:
    for char, pattern in FONT.items():
        assert len(pattern) == FONT_WIDTH, \
            "glyph %r is %d columns, expected %d" % (char, len(pattern), FONT_WIDTH)
        for col in pattern:
            assert col >> FONT_HEIGHT == 0, \
                "glyph %r column %#x is taller than %d rows" % (char, col, FONT_HEIGHT)

    oled = SSD1306(typing.cast(typing.Any, MockI2C()), width=128, height=64)
    assert oled.char('A', 0, 0, True) == CHAR_WIDTH
    assert oled.char('M', 0, 0, True) == CHAR_WIDTH
    assert oled.char('W', 0, 0, True) == CHAR_WIDTH
    assert oled.char(NO_GLYPH, 0, 0, True) == 0  # No glyph, nothing drawn


@test
def test_every_printable_ascii_character_has_a_glyph() -> None:
    for code in range(0x20, 0x7F):
        char = chr(code)
        assert char.upper() in FONT, "no glyph for %r (%#04x)" % (char, code)

    # Lower case is served by the upper case glyphs rather than its own entries.
    assert not [c for c in FONT if c.islower()]
    assert len(FONT) == 0x7F - 0x20 - 26  # All of printable ASCII, less a-z


@test
def test_glyphs_are_distinguishable() -> None:
    seen: dict[tuple, str] = {}
    for char, pattern in FONT.items():
        if char == ' ':
            continue
        key = tuple(pattern)
        # 'O' and '0' share a glyph, as they did before the font was widened.
        assert key not in seen or {seen[key], char} == {'O', '0'}, \
            "glyphs %r and %r are identical" % (seen.get(key), char)
        seen[key] = char


@test
def test_grid_size_follows_display() -> None:
    term = make_term(width=128, height=64)
    assert (term.cols, term.rows) == (25, 8)
    assert (term.text_width, term.text_height) == (125, 64)  # 3 spare pixels on the right
    assert len(term.get_text()) == 8
    assert len(term.get_line(0)) == 25

    small = make_term(width=40, height=24)
    assert (small.cols, small.rows) == (8, 3)
    assert small.get_text() == ["        "] * 3


# ---------------------------------------------------------------------------
# Plain writes
# ---------------------------------------------------------------------------

@test
def test_basic_print() -> None:
    term = make_term()
    term.print("HELLO")
    assert_screen(term, ["HELLO"])
    assert_cursor(term, 5 * CHAR_WIDTH, 0)


@test
def test_println_appends_newline() -> None:
    term = make_term()
    term.println("ONE")
    term.println("TWO")
    assert_screen(term, ["ONE", "TWO"])
    assert_cursor(term, 0, 2 * LINE_HEIGHT)


@test
def test_lowercase_is_folded_to_upper() -> None:
    term = make_term()
    term.print("hello")
    assert_screen(term, ["HELLO"])

    # Every letter must render as its upper case twin, pixels included.
    lower = make_term()
    lower.print("abcdefghijklmnopqrstuvwxyz")
    upper = make_term()
    upper.print("ABCDEFGHIJKLMNOPQRSTUVWXYZ")
    assert lower.get_text() == upper.get_text()
    assert lower.get_buffer() == upper.get_buffer()


@test
def test_characters_without_glyphs_render_blank() -> None:
    # A character outside the font still consumes a cell, but leaves it empty.
    term = make_term()
    term.print("A" + NO_GLYPH + "B")
    assert_screen(term, ["A B"])
    assert_cursor(term, 3 * CHAR_WIDTH, 0)


@test
def test_write_overwrites_previous_cell() -> None:
    term = make_term()
    term.print("ABC\rXY")
    assert_screen(term, ["XYC"])
    assert_cursor(term, 2 * CHAR_WIDTH, 0)

    # A blank-rendering character must erase the glyph underneath it.
    term.print("\r" + NO_GLYPH)
    assert_screen(term, [" YC"])


@test
def test_every_glyph_occupies_one_cell() -> None:
    # M, N and W were the widest glyphs before the font was made fixed width.
    term = make_term()
    term.print("MNW\rABC")
    assert_screen(term, ["ABC"])
    assert_cursor(term, 3 * CHAR_WIDTH, 0)


@test
def test_all_glyphs_round_trip_through_the_text_buffer() -> None:
    term = make_term()
    printable = "".join(c for c in FONT if c != ' ')
    term.print(printable)
    assert "".join(term.get_text()).rstrip() == printable
    assert_consistent(term, "every glyph")


# ---------------------------------------------------------------------------
# Control characters
# ---------------------------------------------------------------------------

@test
def test_newline_and_carriage_return() -> None:
    term = make_term()
    term.print("AB\nCD")
    assert_screen(term, ["AB", "CD"])
    assert_cursor(term, 2 * CHAR_WIDTH, LINE_HEIGHT)

    term.print("\rZ")
    assert_screen(term, ["AB", "ZD"])
    assert_cursor(term, CHAR_WIDTH, LINE_HEIGHT)


@test
def test_backspace_moves_back_one_cell() -> None:
    term = make_term()
    term.print("ABC\b\bZ")
    assert_screen(term, ["AZC"])  # Backspace moves the cursor; it does not erase
    assert_cursor(term, 2 * CHAR_WIDTH, 0)


@test
def test_backspace_stops_at_column_zero() -> None:
    term = make_term()
    term.print("\b\b\bA")
    assert_screen(term, ["A"])
    assert_cursor(term, CHAR_WIDTH, 0)


@test
def test_backspace_does_not_cross_lines() -> None:
    term = make_term()
    term.print("AB\n\b\bZ")
    assert_screen(term, ["AB", "Z"])
    assert_cursor(term, CHAR_WIDTH, LINE_HEIGHT)


@test
def test_backspace_after_tab_moves_one_cell() -> None:
    term = make_term()
    term.print("A\t")
    assert_cursor(term, 8 * CHAR_WIDTH, 0)
    term.print("\bZ")
    assert_cursor(term, 8 * CHAR_WIDTH, 0)
    assert_screen(term, ["A      Z"])


@test
def test_tab_advances_to_next_tab_stop() -> None:
    term = make_term()  # tab_size=8 -> stops every 8 cells
    term.print("A\tB")
    assert_screen(term, ["A       B"])
    assert_cursor(term, 9 * CHAR_WIDTH, 0)

    term.print("\tC")
    assert_screen(term, ["A       B       C"])
    assert_cursor(term, 17 * CHAR_WIDTH, 0)


@test
def test_tab_from_a_tab_stop_advances_a_full_stop() -> None:
    term = make_term()
    term.print("\t")
    assert_cursor(term, 8 * CHAR_WIDTH, 0)
    term.print("\t")
    assert_cursor(term, 16 * CHAR_WIDTH, 0)


@test
def test_tab_size_is_configurable() -> None:
    term = make_term(tab_size=4)
    term.print("A\tB")
    assert_screen(term, ["A   B"])
    assert_cursor(term, 5 * CHAR_WIDTH, 0)


@test
def test_tab_past_the_right_edge_wraps() -> None:
    term = make_term(width=40, height=24)  # 8 columns, so the first tab stop is the edge
    term.print("A\tB")
    assert_screen(term, ["A", "B"])
    assert_cursor(term, CHAR_WIDTH, LINE_HEIGHT)


@test
def test_form_feed_clears_the_screen() -> None:
    term = make_term()
    term.print("ABC\nDEF")
    term.print("\f")
    assert_screen(term, [])
    assert_cursor(term, 0, 0)


@test
def test_bell_is_ignored() -> None:
    term = make_term()
    term.print("A\x07B")
    assert_screen(term, ["AB"])
    assert_cursor(term, 2 * CHAR_WIDTH, 0)


# ---------------------------------------------------------------------------
# Wrapping and scrolling
# ---------------------------------------------------------------------------

@test
def test_line_fills_exactly_without_wrapping() -> None:
    term = make_term()  # 25 columns
    term.print("A" * 25)
    assert_screen(term, ["A" * 25])
    assert_cursor(term, 125, 0)  # Parked at the right edge, wrap deferred


@test
def test_wrap_to_next_line() -> None:
    term = make_term()
    term.print("A" * 25 + "B")
    assert_screen(term, ["A" * 25, "B"])
    assert_cursor(term, CHAR_WIDTH, LINE_HEIGHT)


@test
def test_long_string_wraps_across_lines() -> None:
    term = make_term(width=40, height=24)  # 8x3 grid
    term.print("ABCDEFGHIJKLMNOP")
    assert_screen(term, ["ABCDEFGH", "IJKLMNOP"])
    assert_cursor(term, 8 * CHAR_WIDTH, LINE_HEIGHT)


@test
def test_newline_at_bottom_scrolls() -> None:
    term = make_term(width=40, height=24)
    term.print("A\nB\nC")
    assert_screen(term, ["A", "B", "C"])
    assert_cursor(term, CHAR_WIDTH, 2 * LINE_HEIGHT)

    term.print("\nD")
    assert_screen(term, ["B", "C", "D"])
    assert_cursor(term, CHAR_WIDTH, 2 * LINE_HEIGHT)


@test
def test_wrap_at_bottom_scrolls() -> None:
    term = make_term(width=40, height=24)
    term.print("A" * 8 + "B" * 8 + "C" * 8)
    assert_screen(term, ["A" * 8, "B" * 8, "C" * 8])
    term.print("D")
    assert_screen(term, ["B" * 8, "C" * 8, "D"])
    assert_cursor(term, CHAR_WIDTH, 2 * LINE_HEIGHT)


@test
def test_scroll_up_and_down() -> None:
    term = make_term(width=40, height=24)
    term.print("A\nB\nC")

    term.scroll_up()
    assert_screen(term, ["B", "C", ""])

    term.scroll_down()
    assert_screen(term, ["", "B", "C"])

    term.scroll_down()
    assert_screen(term, ["", "", "B"])


@test
def test_scrolling_preserves_full_rows() -> None:
    term = make_term(width=40, height=24)
    term.print("HELLO\nWORLD\nAGAIN")
    term.scroll_up()
    assert_screen(term, ["WORLD", "AGAIN", ""])


@test
def test_clear_resets_text_and_cursor() -> None:
    term = make_term()
    term.print("ABC\nDEF")
    term.clear()
    assert_screen(term, [])
    assert_cursor(term, 0, 0)


# ---------------------------------------------------------------------------
# CSI: cursor movement
# ---------------------------------------------------------------------------

@test
def test_csi_relative_cursor_movement() -> None:
    term = make_term()

    term.print("\x1b[2B")
    assert_cursor(term, 0, 2 * LINE_HEIGHT, "CUD 2")

    term.print("\x1b[A")
    assert_cursor(term, 0, LINE_HEIGHT, "CUU default")

    term.print("\x1b[10C")
    assert_cursor(term, 10 * CHAR_WIDTH, LINE_HEIGHT, "CUF 10")

    term.print("\x1b[5D")
    assert_cursor(term, 5 * CHAR_WIDTH, LINE_HEIGHT, "CUB 5")


@test
def test_csi_zero_parameter_means_one() -> None:
    term = make_term()
    term.print("\x1b[0B")
    assert_cursor(term, 0, LINE_HEIGHT, "CUD 0")
    term.print("\x1b[0C")
    assert_cursor(term, CHAR_WIDTH, LINE_HEIGHT, "CUF 0")


@test
def test_csi_movement_clamps_at_edges() -> None:
    term = make_term()

    term.print("\x1b[99B")
    assert_cursor(term, 0, 56, "CUD clamped to last row")
    term.print("\x1b[99A")
    assert_cursor(term, 0, 0, "CUU clamped to first row")
    term.print("\x1b[99C")
    assert_cursor(term, 125, 0, "CUF clamped to the last cell boundary")
    term.print("\x1b[99D")
    assert_cursor(term, 0, 0, "CUB clamped to left edge")


@test
def test_csi_cursor_parked_at_right_edge_wraps_on_write() -> None:
    term = make_term()
    term.print("\x1b[99C")
    assert_cursor(term, 125, 0)
    term.print("Z")
    assert_screen(term, ["", "Z"])


@test
def test_csi_next_and_previous_line() -> None:
    term = make_term()
    term.print("AB")
    term.print("\x1b[E")
    assert_cursor(term, 0, LINE_HEIGHT, "CNL")

    term.print("CD\x1b[3E")
    assert_cursor(term, 0, 4 * LINE_HEIGHT, "CNL 3")

    term.print("EF\x1b[2F")
    assert_cursor(term, 0, 2 * LINE_HEIGHT, "CPL 2")

    assert_screen(term, ["AB", "CD", "", "", "EF"])


@test
def test_csi_cursor_horizontal_absolute() -> None:
    term = make_term()
    term.print("\x1b[9G")
    assert_cursor(term, 8 * CHAR_WIDTH, 0, "CHA 9")
    term.print("\x1b[1G")
    assert_cursor(term, 0, 0, "CHA 1")
    term.print("\x1b[G")
    assert_cursor(term, 0, 0, "CHA default")
    term.print("\x1b[99G")
    assert_cursor(term, 125, 0, "CHA clamped")


@test
def test_csi_cursor_position() -> None:
    term = make_term()

    term.print("\x1b[5;8H")
    assert_cursor(term, 7 * CHAR_WIDTH, 4 * LINE_HEIGHT, "CUP 5;8")

    term.print("\x1b[H")
    assert_cursor(term, 0, 0, "CUP home")

    term.print("\x1b[3H")
    assert_cursor(term, 0, 2 * LINE_HEIGHT, "CUP row only")

    term.print("\x1b[;5H")
    assert_cursor(term, 4 * CHAR_WIDTH, 0, "CUP empty row parameter")

    term.print("\x1b[2;3f")
    assert_cursor(term, 2 * CHAR_WIDTH, LINE_HEIGHT, "HVP")

    term.print("\x1b[99;99H")
    assert_cursor(term, 125, 56, "CUP clamped")

    term.print("\x1b[0;0H")
    assert_cursor(term, 0, 0, "CUP zeros mean one")


@test
def test_csi_cursor_position_ignores_extra_parameters() -> None:
    term = make_term()
    term.print("\x1b[2;3;4;5H")
    assert_cursor(term, 2 * CHAR_WIDTH, LINE_HEIGHT)


@test
def test_csi_leading_zeros_in_parameters() -> None:
    term = make_term()
    term.print("\x1b[00005;00008H")
    assert_cursor(term, 7 * CHAR_WIDTH, 4 * LINE_HEIGHT)


@test
def test_csi_writes_land_at_the_positioned_cell() -> None:
    term = make_term(width=40, height=24)
    term.print("\x1b[2;3HXY")
    assert_screen(term, ["", "  XY"])
    assert_cursor(term, 4 * CHAR_WIDTH, LINE_HEIGHT)


@test
def test_csi_save_and_restore_cursor() -> None:
    term = make_term()
    term.print("\x1b[3;4H")
    term.print("\x1b[s")
    term.print("\x1b[H")
    assert_cursor(term, 0, 0, "after home")
    term.print("\x1b[u")
    assert_cursor(term, 3 * CHAR_WIDTH, 2 * LINE_HEIGHT, "after restore")


# ---------------------------------------------------------------------------
# CSI: erasing and scrolling
# ---------------------------------------------------------------------------

def _filled_term() -> Term:
    """An 8x3 grid filled with three known rows, cursor left at row 2, column 5."""
    term = make_term(width=40, height=24)
    term.print("ABCDEFGH\nIJKLMNOP\nQRSTUVWX")
    term.print("\x1b[2;5H")
    assert_cursor(term, 4 * CHAR_WIDTH, LINE_HEIGHT)
    return term


@test
def test_csi_erase_display_to_end() -> None:
    term = _filled_term()
    term.print("\x1b[J")
    assert_screen(term, ["ABCDEFGH", "IJKL", ""])
    assert_cursor(term, 4 * CHAR_WIDTH, LINE_HEIGHT, "ED must not move the cursor")


@test
def test_csi_erase_display_to_start() -> None:
    term = _filled_term()
    term.print("\x1b[1J")
    assert_screen(term, ["", "    MNOP", "QRSTUVWX"])
    assert_cursor(term, 4 * CHAR_WIDTH, LINE_HEIGHT)


@test
def test_csi_erase_display_all() -> None:
    for param in ("2", "3"):
        term = _filled_term()
        term.print("\x1b[%sJ" % param)
        assert_screen(term, [], "ED %s" % param)
        assert_cursor(term, 4 * CHAR_WIDTH, LINE_HEIGHT, "ED %s cursor" % param)


@test
def test_csi_erase_display_at_last_row() -> None:
    term = _filled_term()
    term.print("\x1b[3;1H\x1b[J")
    assert_screen(term, ["ABCDEFGH", "IJKLMNOP", ""])


@test
def test_csi_erase_line_to_end() -> None:
    term = _filled_term()
    term.print("\x1b[K")
    assert_screen(term, ["ABCDEFGH", "IJKL", "QRSTUVWX"])
    assert_cursor(term, 4 * CHAR_WIDTH, LINE_HEIGHT)


@test
def test_csi_erase_line_to_start() -> None:
    term = _filled_term()
    term.print("\x1b[1K")
    assert_screen(term, ["ABCDEFGH", "    MNOP", "QRSTUVWX"])


@test
def test_csi_erase_whole_line() -> None:
    term = _filled_term()
    term.print("\x1b[2K")
    assert_screen(term, ["ABCDEFGH", "", "QRSTUVWX"])


@test
def test_csi_erase_line_at_column_zero() -> None:
    term = _filled_term()
    term.print("\x1b[2;1H\x1b[1K")  # Nothing before the cursor
    assert_screen(term, ["ABCDEFGH", "IJKLMNOP", "QRSTUVWX"])


@test
def test_csi_scroll_up_and_down() -> None:
    term = _filled_term()
    term.print("\x1b[2S")
    assert_screen(term, ["QRSTUVWX", "", ""])

    term = _filled_term()
    term.print("\x1b[T")
    assert_screen(term, ["", "ABCDEFGH", "IJKLMNOP"])

    term = _filled_term()
    term.print("\x1b[S")
    assert_screen(term, ["IJKLMNOP", "QRSTUVWX", ""])


# ---------------------------------------------------------------------------
# CSI: parsing edge cases
# ---------------------------------------------------------------------------

@test
def test_csi_unsupported_sequences_are_ignored() -> None:
    term = make_term()
    term.print("\x1b[31;1mRED\x1b[0m")  # SGR colour
    assert_screen(term, ["RED"])
    assert_cursor(term, 3 * CHAR_WIDTH, 0)

    term.print("\x1b[?25h")   # DECTCEM show cursor (private parameter)
    term.print("\x1b[?25l")
    term.print("\x1b[6n")     # DSR cursor report
    term.print("\x1b[2 q")    # DECSCUSR, with an intermediate byte
    assert_screen(term, ["RED"])
    assert_cursor(term, 3 * CHAR_WIDTH, 0, "ignored sequences must not move the cursor")

    term.print("!")
    assert_screen(term, ["RED!"])


@test
def test_csi_non_numeric_parameter_falls_back_to_default() -> None:
    # '<' is inside the CSI parameter byte range but is not a number.
    term = make_term()
    term.print("\x1b[<B")
    assert_cursor(term, 0, LINE_HEIGHT)


@test
def test_csi_split_across_print_calls() -> None:
    term = make_term()
    term.print("\x1b[")
    term.print("2")
    term.print("B")
    assert_cursor(term, 0, 2 * LINE_HEIGHT, "parser state must survive between print() calls")

    term.print("A\x1b")
    term.print("[2C")
    term.print("B")
    assert_screen(term, ["", "", "A  B"])


@test
def test_escape_without_bracket_is_dropped() -> None:
    term = make_term()
    term.print("\x1bZA")  # ESC swallows 'Z', 'A' prints normally
    assert_screen(term, ["A"])
    assert_cursor(term, CHAR_WIDTH, 0)


@test
def test_escape_sequences_do_not_disturb_surrounding_text() -> None:
    term = make_term(width=40, height=24)
    term.print("AB\x1b[1;5HCD\x1b[2;1HEF")
    assert_screen(term, ["AB  CD", "EF"])
    assert_cursor(term, 2 * CHAR_WIDTH, LINE_HEIGHT)


# ---------------------------------------------------------------------------
# Buffer accessors
# ---------------------------------------------------------------------------

@test
def test_get_text_is_a_snapshot() -> None:
    term = make_term(width=40, height=24)
    term.print("AB")
    snapshot = term.get_text()
    term.print("C")
    assert snapshot[0] == "AB      ", "get_text() must not alias live state"
    assert term.get_text()[0] == "ABC     "


@test
def test_get_line_returns_padded_rows() -> None:
    term = make_term(width=40, height=24)
    term.print("AB\nCD")
    assert term.get_line(0) == "AB      "
    assert term.get_line(1) == "CD      "
    assert term.get_line(2) == "        "


@test
def test_get_buffer_returns_a_copy() -> None:
    term = make_term(width=40, height=24)
    term.print("A")
    copy = term.get_buffer()
    assert copy == term.oled.buffer
    copy[0] = 0xFF
    assert term.oled.buffer[0] != 0xFF, "get_buffer() must return a copy"


@test
def test_auto_show_controls_flushing() -> None:
    oled = SSD1306(typing.cast(typing.Any, MockI2C()), width=40, height=24)
    i2c = typing.cast(typing.Any, oled.i2c)

    quiet = Term(oled, auto_show=False)
    i2c.writes = 0
    quiet.print("ABC")
    assert i2c.writes == 0, "auto_show=False must not touch the bus"
    assert quiet.get_line(0).rstrip() == "ABC", "text is still buffered"

    loud = Term(oled, auto_show=True)
    i2c.writes = 0
    loud.print("D")
    assert i2c.writes > 0, "auto_show=True must flush to the bus"


# ---------------------------------------------------------------------------
# End to end
# ---------------------------------------------------------------------------

@test
def test_mixed_sequence_keeps_text_and_pixels_in_step() -> None:
    term = make_term(width=40, height=24)
    term.print("HELLO\n")
    term.print("WORLD\t!\n")          # The tab runs off the line and wraps
    term.print("\x1b[1;1HXY")
    term.print("\x1b[2;3H\x1b[K")
    term.print("\x1b[3;1HABCDEFGHIJ")  # Wraps and scrolls
    term.print("\b\bZZ")
    term.print("\x1b[s\x1b[1;1HQ\x1b[u")
    assert_screen(term, ["Q", "ABCDEFGH", "ZZ"])
    assert_cursor(term, 2 * CHAR_WIDTH, 2 * LINE_HEIGHT)


@test
def test_repeated_scroll_stays_consistent() -> None:
    term = make_term(width=40, height=24)
    for i in range(20):
        term.println("LINE %d" % (i % 10))
    # The newline that ends the last println scrolls it up, so the bottom row is the
    # empty line the cursor now sits on.
    assert_screen(term, ["LINE 8", "LINE 9", ""])
    assert_cursor(term, 0, 2 * LINE_HEIGHT)


def main() -> int:
    failures = 0
    for fn in TESTS:
        try:
            fn()
        except AssertionError as e:
            failures += 1
            print("FAIL %s\n%s\n" % (fn.__name__, e))
        else:
            print("ok   %s" % fn.__name__)

    print("\n%d test(s), %d failure(s)" % (len(TESTS), failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
