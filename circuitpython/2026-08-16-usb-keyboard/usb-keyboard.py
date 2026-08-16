#!/usr/bin/env circuit-run
"""
USB HID Keyboard controller for Raspberry Pi Pico.

Simulates a USB keyboard sending 'a' and 'b' keystrokes based on physical button inputs.

Pin Connections:
- GP17: Button 1 ('a' key) -> Connected between GP17 and GND (Active LOW, internal pull-up)
- GP18: Button 2 ('b' key) -> Connected between GP18 and GND (Active LOW, internal pull-up)
- Common Ground: Pico GND pin
"""

import time
import board
import digitalio
import usb_hid

# Standard USB HID Keyboard usage page and usage ID
HID_USAGE_PAGE_KEYBOARD: int = 0x01
HID_USAGE_KEYBOARD: int = 0x06

# USB HID Keycodes (Standard USB HID Usage Table)
KEYCODE_A: int = 0x04
KEYCODE_B: int = 0x05

# Debounce duration in seconds
DEBOUNCE_DELAY_SECONDS: float = 0.02


class USBKeyboard:
    """Manages USB HID keyboard reports and key state tracking."""

    def __init__(self) -> None:
        self._device: usb_hid.Device | None = None
        for dev in usb_hid.devices:
            if dev.usage_page == HID_USAGE_PAGE_KEYBOARD and dev.usage == HID_USAGE_KEYBOARD:
                self._device = dev
                break

        if self._device is None:
            raise RuntimeError(
                "USB HID Keyboard interface not available. Ensure USB HID is enabled in CircuitPython."
            )

        self._pressed_keys: set[int] = set()
        self._report: bytearray = bytearray(8)

    def press(self, keycode: int) -> None:
        """Adds a keycode to the active report and sends the update to the host."""
        if keycode not in self._pressed_keys:
            self._pressed_keys.add(keycode)
            self._send_report()

    def release(self, keycode: int) -> None:
        """Removes a keycode from the active report and sends the update to the host."""
        if keycode in self._pressed_keys:
            self._pressed_keys.remove(keycode)
            self._send_report()

    def release_all(self) -> None:
        """Clears all pressed keys and sends an empty report."""
        if self._pressed_keys:
            self._pressed_keys.clear()
            self._send_report()

    def _send_report(self) -> None:
        """Constructs and transmits the standard 8-byte HID keyboard report."""
        for i in range(8):
            self._report[i] = 0

        # Byte 0: Modifiers (0 for standard keys)
        # Byte 1: Reserved (0x00)
        # Bytes 2-7: Up to 6 concurrent keycodes
        for index, keycode in enumerate(sorted(self._pressed_keys)):
            if index < 6:
                self._report[2 + index] = keycode

        self._device.send_report(self._report)


class DebouncedButton:
    """Debounces digital input transitions for a physical button with pull-up."""

    def __init__(self, pin: board.Pin, debounce_delay_s: float = DEBOUNCE_DELAY_SECONDS) -> None:
        self.io: digitalio.DigitalInOut = digitalio.DigitalInOut(pin)
        self.io.direction = digitalio.Direction.INPUT
        self.io.pull = digitalio.Pull.UP
        self.debounce_delay_s: float = debounce_delay_s

        # Active LOW: False when button is pressed, True when released
        self.is_pressed: bool = not self.io.value
        self._last_raw_value: bool = self.io.value
        self._last_change_time: float = time.monotonic()

    def update(self) -> bool:
        """
        Polls the button state with time-based debouncing.
        Returns True if the debounced pressed state changed, False otherwise.
        """
        raw_val: bool = self.io.value
        now: float = time.monotonic()

        if raw_val != self._last_raw_value:
            self._last_raw_value = raw_val
            self._last_change_time = now

        if (now - self._last_change_time) >= self.debounce_delay_s:
            debounced_pressed: bool = not raw_val
            if debounced_pressed != self.is_pressed:
                self.is_pressed = debounced_pressed
                return True

        return False


def main() -> None:
    """Initializes hardware peripherals and runs the main keyboard event loop."""
    print("Initializing USB Keyboard...")
    keyboard: USBKeyboard = USBKeyboard()

    # Configure buttons on GP17 ('a') and GP18 ('b')
    button_a: DebouncedButton = DebouncedButton(board.GP17)
    button_b: DebouncedButton = DebouncedButton(board.GP18)

    # Configure onboard LED for visual activity indicator
    led: digitalio.DigitalInOut | None = None
    try:
        led = digitalio.DigitalInOut(board.LED)
        led.direction = digitalio.Direction.OUTPUT
        led.value = False
    except Exception:
        # Some boards or configurations may not expose board.LED
        pass

    print("USB Keyboard ready.")
    print("Press GP17 for 'a', GP18 for 'b'.")

    while True:
        # Check Button A (GP17 -> 'a')
        if button_a.update():
            if button_a.is_pressed:
                keyboard.press(KEYCODE_A)
                print("Key 'A' PRESSED (GP17)")
            else:
                keyboard.release(KEYCODE_A)
                print("Key 'A' RELEASED (GP17)")

        # Check Button B (GP18 -> 'b')
        if button_b.update():
            if button_b.is_pressed:
                keyboard.press(KEYCODE_B)
                print("Key 'B' PRESSED (GP18)")
            else:
                keyboard.release(KEYCODE_B)
                print("Key 'B' RELEASED (GP18)")

        # Update onboard LED indicator when any button is pressed
        if led is not None:
            led.value = button_a.is_pressed or button_b.is_pressed

        # Short sleep to prevent busy-waiting while keeping low latency
        time.sleep(0.002)


if __name__ == "__main__":
    main()
