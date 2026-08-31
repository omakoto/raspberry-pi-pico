# I2CKeyPad CircuitPython Driver & Test

A CircuitPython port of [Rob Tillaart's I2CKeyPad library](https://github.com/RobTillaart/I2CKeyPad) for interfacing matrix keypads (4x4, 5x3, 6x2, 8x1) using a **PCF8574** / **PCF8574A** I2C I/O expander.

---

## Hardware Setup

### Wiring Diagram

```
      ESP32-S3 / MCU               PCF8574                  4x4 Keypad
    +----------------+        +---------------+          +---------------+
    |                |        |           P0  |<-------->| Row 0         |
    |   SDA (D4/IO5) |<------>| SDA       P1  |<-------->| Row 1         |
    |   SCL (D5/IO6) |------->| SCL       P2  |<-------->| Row 2         |
    |                |        |           P3  |<-------->| Row 3         |
    |           3.3V |------->| VCC           |          |               |
    |            GND |------->| GND       P4  |<-------->| Col 0         |
    |                |        |           P5  |<-------->| Col 1         |
    |                |        |           P6  |<-------->| Col 2         |
    |                |        |           P7  |<-------->| Col 3         |
    +----------------+        +---------------+          +---------------+
```

### Pin Assignment

* **ESP32-S3 (e.g. Seeed XIAO ESP32-S3)**:
  * `SDA` -> `D4` (`GPIO5`)
  * `SCL` -> `D5` (`GPIO6`)
* **Raspberry Pi Pico**:
  * `SDA` -> `GP10` / `GP4`
  * `SCL` -> `GP11` / `GP5`
* **PCF8574 I2C Address**:
  * Default `0x20` (PCF8574) or `0x38` (PCF8574A). Configurable via jumpers A0, A1, A2.

---

## Keypad Layout & Keymap

Standard 4x4 Matrix mapping:

```
+---+---+---+---+
| 1 | 2 | 3 | A |  -> Row 0 (Keys 0, 1, 2, 3)
+---+---+---+---+
| 4 | 5 | 6 | B |  -> Row 1 (Keys 4, 5, 6, 7)
+---+---+---+---+
| 7 | 8 | 9 | C |  -> Row 2 (Keys 8, 9, 10, 11)
+---+---+---+---+
| * | 0 | # | D |  -> Row 3 (Keys 12, 13, 14, 15)
+---+---+---+---+
```

Keymap format: 16 characters for keys `0..15`, followed by character 16 for `NOKEY` (`'N'`) and character 17 for `FAIL` (`'F'`).

```python
DEFAULT_KEYMAP_4x4 = "123A456B789C*0#DNF"
```

---

## Features

- **Matrix Scanning**: Supports 4x4, 5x3, 6x2, and 8x1 keypad configurations.
- **Debounce Threshold**: Configurable threshold in milliseconds to eliminate mechanical contact bounce.
- **Keypress Detection**: Fast `is_pressed()` check to detect active presses without a full matrix scan.
- **Key Translation**: `get_key()` returns raw integer index; `get_char()` returns mapped character.
- **State Memory**: `get_last_key()` and `get_last_char()` track the most recent valid keypress.
- **Zero External Dependencies**: Works directly with CircuitPython's built-in `busio.I2C`.

---

## API Reference

### Initialization & Configuration

```python
from i2ckeypad import I2CKeyPad, KEYPAD_4x4

# Initialize driver on I2C bus
keypad = I2CKeyPad(i2c, address=0x20)

# Connect and set mode
keypad.begin(KEYPAD_4x4)

# Set custom character mapping
keypad.load_keymap("123A456B789C*0#DNF")

# Set debounce threshold (in milliseconds)
keypad.set_debounce_threshold(50)
```

### Reading Keys

```python
# Check if any key is currently pressed
if keypad.is_pressed():
    print("Key is held down")

# Read raw key index (0-15, KEYPAD_NOKEY=16, KEYPAD_FAIL=17, KEYPAD_THRESHOLD=255)
key_idx = keypad.get_key()

# Read mapped character
char_val = keypad.get_char()

# Get last valid key pressed
last_key = keypad.get_last_key()
last_char = keypad.get_last_char()
```

---

## Running the Demo

Deploy to connected board:

```bash
circuit-run i2ckeypad-test.py
```

Run host-side unit tests:

```bash
python3 test_i2ckeypad.py
```
