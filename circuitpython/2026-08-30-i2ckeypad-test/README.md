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

The script requests `D4` (SDA) and `D5` (SCL); `common.get_pin()` resolves those
names to the equivalent pins on whichever board is running.

#### Raspberry Pi Pico (Pico / Pico W / Pico 2 / Pico 2 W)

| Function | Pin Name | Default GPIO | Physical Pin # | Details |
| :--- | :--- | :--- | :--- | :--- |
| **I2C SDA** (PCF8574 `SDA`) | GP4 | `GPIO4` | Pin 6 | I2C0 data line |
| **I2C SCL** (PCF8574 `SCL`) | GP5 | `GPIO5` | Pin 7 | I2C0 clock line |
| **3.3V Power** (PCF8574 `VCC`) | 3V3(OUT) | `3V3` | Pin 36 | 3.3V DC power for the expander and keypad |
| **Ground** (PCF8574 `GND`) | GND | `GND` | Pin 3, 8, 13, 18, 23, 28, 38 | Common digital ground |

To drive the expander from I2C1 instead, set `PIN_I2C_SDA` / `PIN_I2C_SCL` to
`GP10` (Pin 14) and `GP11` (Pin 15).

#### ESP32-S3

| Function | ESP32-S3 GPIO | DevKitC-1 Physical Pin | Seeed Studio XIAO Pin | Details |
| :--- | :---: | :---: | :---: | :--- |
| **I2C SDA** (PCF8574 `SDA`) | `GPIO5` | Row B, Pin 5 (silk `5`) | `D4` (Pin 5) | Hardware I2C data line |
| **I2C SCL** (PCF8574 `SCL`) | `GPIO6` | Row B, Pin 6 (silk `6`) | `D5` (Pin 6) | Hardware I2C clock line |
| **3.3V Power** (PCF8574 `VCC`) | `3V3` | Row B, Pin 1 or 2 (silk `3V3`) | `3V3` (Pin 12) | 3.3V DC power for the expander and keypad |
| **Ground** (PCF8574 `GND`) | `GND` | Row A, Pin 1/21/22 or Row B, Pin 22 (silk `G`) | `GND` (Pin 13) | Common digital ground |

**PCF8574 I2C Address**: default `0x20` (PCF8574) or `0x38` (PCF8574A), configurable via jumpers A0, A1, A2.

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
