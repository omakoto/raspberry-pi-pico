# Driver library for matrix keypads (4x4, 5x3, 6x2, 8x1) connected via PCF8574 / PCF8574A I2C expander.
# CircuitPython port of Rob Tillaart's I2CKeyPad library (https://github.com/RobTillaart/I2CKeyPad).

import time

try:
    import busio
except ImportError:
    # Dummy mock for host-side unit testing environments
    class _DummyBusIO:
        class I2C:
            pass

    busio = _DummyBusIO  # type: ignore

# Keypad Return Constants
KEYPAD_NOKEY: int = 16
KEYPAD_FAIL: int = 17
KEYPAD_THRESHOLD: int = 255

# Keypad Layout Modes
KEYPAD_4x4: int = 44
KEYPAD_5x3: int = 53
KEYPAD_6x2: int = 62
KEYPAD_8x1: int = 81

# Default keymap for standard 4x4 matrix: 16 keys + NOKEY ('N') + FAIL ('F')
DEFAULT_KEYMAP_4x4: str = "123A456B789C*0#DNF"


class I2CKeyPad:
    # CircuitPython driver for reading matrix keypads using PCF8574/PCF8574A I2C I/O expander.

    def __init__(
        self,
        i2c: busio.I2C,
        address: int = 0x20,
        reverse_row: bool = False,
        reverse_col: bool = False,
    ) -> None:
        # Initialize the I2CKeyPad instance.
        self._i2c: busio.I2C = i2c
        self._address: int = address
        self._mode: int = KEYPAD_4x4
        self._reverse_row: bool = reverse_row
        self._reverse_col: bool = reverse_col
        self._keymap: str | list[str] | None = None
        self._last_key: int = KEYPAD_NOKEY
        self._last_char: str = "N"
        self._last_read_time_ms: int = 0
        self._debounce_threshold_ms: int = 0

    def begin(self, mode: int = KEYPAD_4x4) -> bool:
        # Configures the keypad mode and initializes the PCF8574 expander.
        self._mode = mode
        if not self.is_connected():
            return False
        # Set idle state on PCF8574
        self._reset_bus()
        return True

    def is_connected(self) -> bool:
        # Checks if the PCF8574 device is responding at the configured I2C address.
        while not self._i2c.try_lock():
            pass
        try:
            # Probe address with standard idle write
            self._i2c.writeto(self._address, bytes([0xF0]))
            return True
        except Exception:
            return False
        finally:
            self._i2c.unlock()

    def set_keypad_mode(self, mode: int = KEYPAD_4x4) -> None:
        # Sets the keypad matrix dimension mode (KEYPAD_4x4, KEYPAD_5x3, KEYPAD_6x2, KEYPAD_8x1).
        self._mode = mode
        self._reset_bus()

    def get_keypad_mode(self) -> int:
        # Returns the current keypad mode.
        return self._mode

    def set_reverse_row(self, reverse: bool) -> None:
        # Enables or disables row order reversal.
        self._reverse_row = reverse

    def get_reverse_row(self) -> bool:
        # Returns True if row order is reversed.
        return self._reverse_row

    def set_reverse_col(self, reverse: bool) -> None:
        # Enables or disables column order reversal.
        self._reverse_col = reverse

    def get_reverse_col(self) -> bool:
        # Returns True if column order is reversed.
        return self._reverse_col

    def load_keymap(self, keymap: str | list[str]) -> None:
        # Loads a key character mapping string or list.
        self._keymap = keymap

    def get_keymap(self) -> str | list[str] | None:
        # Returns the currently loaded key mapping.
        return self._keymap

    def set_debounce_threshold(self, threshold_ms: int) -> None:
        # Sets the debounce threshold in milliseconds (0 disables threshold check).
        self._debounce_threshold_ms = threshold_ms

    def get_debounce_threshold(self) -> int:
        # Returns the debounce threshold in milliseconds.
        return self._debounce_threshold_ms

    def is_pressed(self) -> bool:
        # Checks whether any key is currently pressed on the keypad.
        try:
            if self._mode == KEYPAD_4x4:
                val: int = self._read_raw(0xF0)
                return (val & 0xF0) != 0xF0
            elif self._mode == KEYPAD_5x3:
                val = self._read_raw(0xE0)
                return (val & 0xE0) != 0xE0
            elif self._mode == KEYPAD_6x2:
                val = self._read_raw(0xC0)
                return (val & 0xC0) != 0xC0
            elif self._mode == KEYPAD_8x1:
                val = self._read_raw(0xFF)
                return val != 0xFF
        except Exception:
            pass
        return False

    def get_key(self) -> int:
        # Scans the matrix and returns the raw key index (0-15), KEYPAD_NOKEY, KEYPAD_FAIL, or KEYPAD_THRESHOLD.
        now_ms: int = int(time.monotonic() * 1000)
        if self._debounce_threshold_ms > 0:
            if (now_ms - self._last_read_time_ms) < self._debounce_threshold_ms:
                return KEYPAD_THRESHOLD

        key: int = KEYPAD_NOKEY
        try:
            if self._mode == KEYPAD_4x4:
                key = self._get_key_4x4()
            elif self._mode == KEYPAD_5x3:
                key = self._get_key_5x3()
            elif self._mode == KEYPAD_6x2:
                key = self._get_key_6x2()
            elif self._mode == KEYPAD_8x1:
                key = self._get_key_8x1()
        except Exception:
            key = KEYPAD_FAIL

        if key != KEYPAD_NOKEY and key != KEYPAD_FAIL:
            self._last_key = key
            self._last_read_time_ms = now_ms
            if self._keymap is not None and key < len(self._keymap):
                self._last_char = str(self._keymap[key])

        return key

    def key_to_char(self, key: int) -> str | int:
        # Translates a key index to its corresponding character based on the loaded keymap.
        if self._keymap is None:
            return key
        if key == KEYPAD_NOKEY:
            return self._keymap[16] if len(self._keymap) > 16 else "N"
        if key == KEYPAD_FAIL:
            return self._keymap[17] if len(self._keymap) > 17 else "F"
        if 0 <= key < len(self._keymap):
            return str(self._keymap[key])
        return key

    def get_char(self) -> str | int:
        # Scans the matrix and returns the mapped character, or KEYPAD_THRESHOLD if debouncing.
        key: int = self.get_key()
        if key == KEYPAD_THRESHOLD:
            return KEYPAD_THRESHOLD
        return self.key_to_char(key)

    def get_last_key(self) -> int:
        # Returns the last valid pressed key index.
        return self._last_key

    def get_last_char(self) -> str:
        # Returns the last valid pressed character according to the loaded keymap.
        return self._last_char

    # Internal scanning implementations

    def _reset_bus(self) -> None:
        # Restores the idle state output on the PCF8574 expander.
        if self._mode == KEYPAD_4x4:
            self._write_raw(0xF0)
        elif self._mode == KEYPAD_5x3:
            self._write_raw(0xE0)
        elif self._mode == KEYPAD_6x2:
            self._write_raw(0xC0)
        elif self._mode == KEYPAD_8x1:
            self._write_raw(0xFF)

    def _write_raw(self, value: int) -> None:
        # Writes a single raw byte to the PCF8574 expander.
        while not self._i2c.try_lock():
            pass
        try:
            self._i2c.writeto(self._address, bytes([value & 0xFF]))
        finally:
            self._i2c.unlock()

    def _read_raw(self, mask: int) -> int:
        # Writes mask to expander and immediately reads the resulting input state byte.
        while not self._i2c.try_lock():
            pass
        try:
            self._i2c.writeto(self._address, bytes([mask & 0xFF]))
            buf: bytearray = bytearray(1)
            self._i2c.readfrom_into(self._address, buf)
            return buf[0]
        finally:
            self._i2c.unlock()

    def _get_key_4x4(self) -> int:
        # Scans 4 rows (P0-P3) and 4 columns (P4-P7).
        # Step 1: Read columns with rows held LOW
        col_byte: int = self._read_raw(0xF0)
        col_val: int = (col_byte >> 4) & 0x0F
        if col_val == 0x0F:
            return KEYPAD_NOKEY

        col: int = -1
        if col_val == 0x0E:
            col = 0
        elif col_val == 0x0D:
            col = 1
        elif col_val == 0x0B:
            col = 2
        elif col_val == 0x07:
            col = 3
        else:
            self._write_raw(0xF0)
            return KEYPAD_FAIL

        # Step 2: Read rows with columns held LOW
        row_byte: int = self._read_raw(0x0F)
        row_val: int = row_byte & 0x0F
        if row_val == 0x0F:
            self._write_raw(0xF0)
            return KEYPAD_NOKEY

        row: int = -1
        if row_val == 0x0E:
            row = 0
        elif row_val == 0x0D:
            row = 1
        elif row_val == 0x0B:
            row = 2
        elif row_val == 0x07:
            row = 3
        else:
            self._write_raw(0xF0)
            return KEYPAD_FAIL

        # Reset PCF8574 back to idle state
        self._write_raw(0xF0)

        # Apply reversal if configured
        if self._reverse_col:
            col = 3 - col
        if self._reverse_row:
            row = 3 - row

        return (row * 4) + col

    def _get_key_5x3(self) -> int:
        # Scans 5 rows (P0-P4) and 3 columns (P5-P7).
        col_byte: int = self._read_raw(0xE0)
        col_val: int = (col_byte >> 5) & 0x07
        if col_val == 0x07:
            return KEYPAD_NOKEY

        col: int = -1
        if col_val == 0x06:
            col = 0
        elif col_val == 0x05:
            col = 1
        elif col_val == 0x03:
            col = 2
        else:
            self._write_raw(0xE0)
            return KEYPAD_FAIL

        row_byte: int = self._read_raw(0x1F)
        row_val: int = row_byte & 0x1F
        if row_val == 0x1F:
            self._write_raw(0xE0)
            return KEYPAD_NOKEY

        row: int = -1
        if row_val == 0x1E:
            row = 0
        elif row_val == 0x1D:
            row = 1
        elif row_val == 0x1B:
            row = 2
        elif row_val == 0x17:
            row = 3
        elif row_val == 0x0F:
            row = 4
        else:
            self._write_raw(0xE0)
            return KEYPAD_FAIL

        self._write_raw(0xE0)

        if self._reverse_col:
            col = 2 - col
        if self._reverse_row:
            row = 4 - row

        return (row * 3) + col

    def _get_key_6x2(self) -> int:
        # Scans 6 rows (P0-P5) and 2 columns (P6-P7).
        col_byte: int = self._read_raw(0xC0)
        col_val: int = (col_byte >> 6) & 0x03
        if col_val == 0x03:
            return KEYPAD_NOKEY

        col: int = -1
        if col_val == 0x02:
            col = 0
        elif col_val == 0x01:
            col = 1
        else:
            self._write_raw(0xC0)
            return KEYPAD_FAIL

        row_byte: int = self._read_raw(0x3F)
        row_val: int = row_byte & 0x3F
        if row_val == 0x3F:
            self._write_raw(0xC0)
            return KEYPAD_NOKEY

        row: int = -1
        if row_val == 0x3E:
            row = 0
        elif row_val == 0x3D:
            row = 1
        elif row_val == 0x3B:
            row = 2
        elif row_val == 0x37:
            row = 3
        elif row_val == 0x2F:
            row = 4
        elif row_val == 0x1F:
            row = 5
        else:
            self._write_raw(0xC0)
            return KEYPAD_FAIL

        self._write_raw(0xC0)

        if self._reverse_col:
            col = 1 - col
        if self._reverse_row:
            row = 5 - row

        return (row * 2) + col

    def _get_key_8x1(self) -> int:
        # Scans 8 buttons (P0-P7) connected to ground.
        row_byte: int = self._read_raw(0xFF)
        if row_byte == 0xFF:
            return KEYPAD_NOKEY

        lookup: dict[int, int] = {
            0xFE: 0,
            0xFD: 1,
            0xFB: 2,
            0xF7: 3,
            0xEF: 4,
            0xDF: 5,
            0xBF: 6,
            0x7F: 7,
        }
        if row_byte in lookup:
            key: int = lookup[row_byte]
            if self._reverse_row:
                key = 7 - key
            return key
        return KEYPAD_FAIL
