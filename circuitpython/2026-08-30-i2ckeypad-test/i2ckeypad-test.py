#!/usr/bin/env circuit-run
#file: ../libs/common.py
#file: i2ckeypad.py
#
# Test and demo script for I2C PCF8574-based matrix keypads on ESP32-S3 / Raspberry Pi Pico.
# Reads pressed keys and logs both the raw key index and the mapped character in real time.
#
# Hardware Connections (ESP32-S3 / XIAO ESP32-S3):
# - SDA: D4 (IO5)
# - SCL: D5 (IO6)
# - VCC: 3.3V (or 5V if PCF8574 module requires 5V logic)
# - GND: GND
# - PCF8574 I2C Address: 0x20 (Default)

import time
import board
import digitalio
from common import get_i2c, get_led_pin
from i2ckeypad import (
    I2CKeyPad,
    KEYPAD_4x4,
    KEYPAD_NOKEY,
    KEYPAD_FAIL,
    KEYPAD_THRESHOLD,
    DEFAULT_KEYMAP_4x4,
)

# Pin Definitions
PIN_I2C_SDA: int | str | None = "D4"
PIN_I2C_SCL: int | str | None = "D5"

# PCF8574 I2C Address (0x20 to 0x27 for PCF8574, 0x38 to 0x3F for PCF8574A)
I2C_ADDRESS: int = 0x20

# Keypad Orientation Adjustments (flip rows and/or columns to match physical keypad header pinout)
REVERSE_ROW: bool = True
REVERSE_COL: bool = True

# 4x4 Keypad Character Map (16 keys + NOKEY 'N' + FAIL 'F')
# Layout:
# 1 2 3 A (keys 0, 1, 2, 3)
# 4 5 6 B (keys 4, 5, 6, 7)
# 7 8 9 C (keys 8, 9, 10, 11)
# * 0 # D (keys 12, 13, 14, 15)
KEYPAD_MAP: str = DEFAULT_KEYMAP_4x4

# Debounce threshold in milliseconds (0 disables threshold check)
DEBOUNCE_THRESHOLD_MS: int = 50

# Visual activity LED indicator
led_pin: board.Pin | None = get_led_pin()
led: digitalio.DigitalInOut | None = None
if led_pin is not None:
    try:
        led = digitalio.DigitalInOut(led_pin)
        led.direction = digitalio.Direction.OUTPUT
        # Blink 3 times on startup
        for _ in range(3):
            led.value = True
            time.sleep(0.1)
            led.value = False
            time.sleep(0.1)
    except Exception:
        led = None

print("=" * 50)
print("I2CKeyPad (PCF8574) CircuitPython Test")
print(f"I2C Pins -> SDA: {PIN_I2C_SDA}, SCL: {PIN_I2C_SCL}")
print(f"I2C Target Address: {hex(I2C_ADDRESS)}")
print(f"Reversal -> Row: {REVERSE_ROW}, Col: {REVERSE_COL}")
print("=" * 50)

# Initialize I2C bus with retry
i2c = None
while i2c is None:
    try:
        i2c = get_i2c(scl=PIN_I2C_SCL, sda=PIN_I2C_SDA)
        print("I2C bus initialized successfully.")
    except Exception as e:
        print(f"I2C initialization failed: {e}. Retrying in 2 seconds...")
        time.sleep(2.0)

# Diagnostic bus scan
if i2c.try_lock():
    try:
        addresses: list[int] = i2c.scan()
        hex_addrs: list[str] = [hex(a) for a in addresses]
        print(f"I2C scan discovered devices: {hex_addrs}")
        if I2C_ADDRESS in addresses:
            print(f"PCF8574 detected at address {hex(I2C_ADDRESS)}.")
        else:
            print(f"Warning: Address {hex(I2C_ADDRESS)} not detected in I2C scan.")
            print("Check SDA/SCL wiring, power connections, or check address jumpers (A0-A2).")
    except Exception as e:
        print(f"I2C scan error: {e}")
    finally:
        i2c.unlock()

# Initialize the I2CKeyPad instance
keypad: I2CKeyPad = I2CKeyPad(
    i2c,
    address=I2C_ADDRESS,
    reverse_row=REVERSE_ROW,
    reverse_col=REVERSE_COL,
)
keypad.load_keymap(KEYPAD_MAP)
keypad.set_debounce_threshold(DEBOUNCE_THRESHOLD_MS)

if not keypad.begin(KEYPAD_4x4):
    print(f"Warning: keypad.begin() reported device not responding at {hex(I2C_ADDRESS)}.")
else:
    print("I2CKeyPad initialized successfully. Ready to read keys.")

print("\nPress keys on the keypad...")
print("-" * 50)

last_reported_key: int = KEYPAD_NOKEY

while True:
    # Read key index and mapped character
    key_idx: int = keypad.get_key()

    if key_idx == KEYPAD_THRESHOLD:
        # Debounce period active
        time.sleep(0.01)
        continue

    if key_idx != last_reported_key:
        if key_idx == KEYPAD_NOKEY:
            # Key released
            if led is not None:
                led.value = False
        elif key_idx == KEYPAD_FAIL:
            print("[KEY EVENT] Multiple keys pressed or I2C read error (FAIL)")
        else:
            # Valid key pressed
            char_val: str | int = keypad.key_to_char(key_idx)
            row: int = key_idx // 4
            col: int = key_idx % 4
            print(f"[KEY PRESS] Key: '{char_val}' | Index: {key_idx:2d} | Row: {row} | Col: {col}")

            if led is not None:
                led.value = True

        last_reported_key = key_idx

    time.sleep(0.02)
