#!/usr/bin/env python3
# Unit tests for the CircuitPython I2CKeyPad driver.
# Validates key matrix scanning, layout modes (4x4, 5x3, 6x2, 8x1), keymap translation, and debounce logic.

import unittest
import sys
import os

# Ensure local directory is on sys.path
sys.path.insert(0, os.path.dirname(__file__))

from i2ckeypad import (
    I2CKeyPad,
    KEYPAD_4x4,
    KEYPAD_5x3,
    KEYPAD_6x2,
    KEYPAD_8x1,
    KEYPAD_NOKEY,
    KEYPAD_FAIL,
    KEYPAD_THRESHOLD,
    DEFAULT_KEYMAP_4x4,
)


class MockI2C:
    # Mock busio.I2C implementation for host-side unit testing.
    def __init__(self) -> None:
        self.locked: bool = False
        self.written_bytes: list[bytes] = []
        self.read_responses: list[bytes] = []
        self.read_index: int = 0

    def try_lock(self) -> bool:
        self.locked = True
        return True

    def unlock(self) -> None:
        self.locked = False

    def writeto(self, address: int, data: bytes) -> None:
        self.written_bytes.append(bytes(data))

    def readfrom_into(self, address: int, buffer: bytearray) -> None:
        if self.read_index < len(self.read_responses):
            resp = self.read_responses[self.read_index]
            self.read_index += 1
            for i in range(min(len(buffer), len(resp))):
                buffer[i] = resp[i]
        else:
            for i in range(len(buffer)):
                buffer[i] = 0xFF

    def queue_read(self, *responses: bytes | int) -> None:
        for r in responses:
            if isinstance(r, int):
                self.read_responses.append(bytes([r]))
            else:
                self.read_responses.append(r)


class TestI2CKeyPad(unittest.TestCase):

    def setUp(self) -> None:
        self.mock_i2c = MockI2C()
        self.keypad = I2CKeyPad(self.mock_i2c, address=0x20)

    def test_begin_and_connection(self) -> None:
        self.assertTrue(self.keypad.begin(KEYPAD_4x4))
        self.assertTrue(self.keypad.is_connected())

    def test_4x4_single_keys(self) -> None:
        self.keypad.load_keymap(DEFAULT_KEYMAP_4x4)
        # 4x4 matrix:
        # Col 0 (P4 low) -> col byte: 0b1110_xxxx -> 0xEF or 0xEE (upper nibble 0xE)
        # Row 0 (P0 low) -> row byte: 0bxxxx_1110 -> 0xFE or 0xEE (lower nibble 0xE)
        # Key 0 -> row 0, col 0 -> '1'
        self.mock_i2c.queue_read(0xE0, 0x0E)
        self.assertEqual(self.keypad.get_key(), 0)
        self.assertEqual(self.keypad.get_last_char(), "1")

        # Key 5 -> row 1, col 1 -> '5' (row_val 0xD, col_val 0xD)
        self.mock_i2c.queue_read(0xD0, 0x0D)
        self.assertEqual(self.keypad.get_key(), 5)
        self.assertEqual(self.keypad.get_last_char(), "5")

        # Key 15 -> row 3, col 3 -> 'D' (row_val 0x7, col_val 0x7)
        self.mock_i2c.queue_read(0x70, 0x07)
        self.assertEqual(self.keypad.get_key(), 15)
        self.assertEqual(self.keypad.get_last_char(), "D")

    def test_4x4_reversed_keys(self) -> None:
        self.keypad.set_reverse_row(True)
        self.keypad.set_reverse_col(True)
        self.keypad.load_keymap(DEFAULT_KEYMAP_4x4)

        # Physical press at top-left ('1'): hardware scan returns row 3, col 3 (0x70, 0x07)
        # With reversal, it should map to row 0, col 0 -> key index 0 -> '1'
        self.mock_i2c.queue_read(0x70, 0x07)
        self.assertEqual(self.keypad.get_key(), 0)
        self.assertEqual(self.keypad.get_last_char(), "1")

        # Physical press at bottom-right ('D'): hardware scan returns row 0, col 0 (0xE0, 0x0E)
        # With reversal, it should map to row 3, col 3 -> key index 15 -> 'D'
        self.mock_i2c.queue_read(0xE0, 0x0E)
        self.assertEqual(self.keypad.get_key(), 15)
        self.assertEqual(self.keypad.get_last_char(), "D")

    def test_no_key_pressed(self) -> None:
        self.keypad.load_keymap(DEFAULT_KEYMAP_4x4)
        # All columns high (0xF0)
        self.mock_i2c.queue_read(0xF0)
        self.assertEqual(self.keypad.get_key(), KEYPAD_NOKEY)

        self.mock_i2c.queue_read(0xF0)
        self.assertEqual(self.keypad.get_char(), "N")

    def test_multiple_keys_fail(self) -> None:
        self.keypad.load_keymap(DEFAULT_KEYMAP_4x4)
        # Two columns low (0b1100_0000 -> 0xC0)
        self.mock_i2c.queue_read(0xC0)
        self.assertEqual(self.keypad.get_key(), KEYPAD_FAIL)

        self.mock_i2c.queue_read(0xC0)
        self.assertEqual(self.keypad.get_char(), "F")

    def test_key_to_char(self) -> None:
        self.keypad.load_keymap(DEFAULT_KEYMAP_4x4)
        self.assertEqual(self.keypad.key_to_char(0), "1")
        self.assertEqual(self.keypad.key_to_char(3), "A")
        self.assertEqual(self.keypad.key_to_char(KEYPAD_NOKEY), "N")
        self.assertEqual(self.keypad.key_to_char(KEYPAD_FAIL), "F")

    def test_is_pressed(self) -> None:
        # Not pressed
        self.mock_i2c.queue_read(0xF0)
        self.assertFalse(self.keypad.is_pressed())

        # Pressed
        self.mock_i2c.queue_read(0xE0)
        self.assertTrue(self.keypad.is_pressed())

    def test_5x3_mode(self) -> None:
        self.keypad.set_keypad_mode(KEYPAD_5x3)
        # Col 2 (bit 7 low: 0b011x_xxxx -> 0x60 -> (0x60>>5)&7 = 3 (col 2))
        # Row 4 (bit 4 low: 0bxxx0_1111 -> 0x0F -> row 4)
        # Key = row 4 * 3 + col 2 = 14
        self.mock_i2c.queue_read(0x60, 0x0F)
        self.assertEqual(self.keypad.get_key(), 14)

    def test_8x1_mode(self) -> None:
        self.keypad.set_keypad_mode(KEYPAD_8x1)
        # Button 0 (P0 low -> 0xFE)
        self.mock_i2c.queue_read(0xFE)
        self.assertEqual(self.keypad.get_key(), 0)

        # Button 7 (P7 low -> 0x7F)
        self.mock_i2c.queue_read(0x7F)
        self.assertEqual(self.keypad.get_key(), 7)

    def test_debounce_threshold(self) -> None:
        self.keypad.set_debounce_threshold(500)
        self.assertEqual(self.keypad.get_debounce_threshold(), 500)

        # First key read
        self.mock_i2c.queue_read(0xE0, 0x0E)
        self.assertEqual(self.keypad.get_key(), 0)

        # Immediate second read within debounce window -> THRESHOLD
        self.mock_i2c.queue_read(0xE0, 0x0E)
        self.assertEqual(self.keypad.get_key(), KEYPAD_THRESHOLD)


if __name__ == "__main__":
    unittest.main()
