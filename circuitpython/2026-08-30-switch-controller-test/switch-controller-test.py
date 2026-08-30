#!/usr/bin/env circuit-run
#file: ../libs/common.py
#file: boot.py
#
# Switch Controller Emulator (Composite Device Mode) for Raspberry Pi Pico, ESP32, and compatible boards.
#
# Emulates a HORI Pokken Nintendo Switch controller over USB HID.
# Reads physical inputs and transmits 8-byte HID reports to the Nintendo Switch or host PC:
# - Pin 0 (D0 / GP0): A button (Active LOW, internal pull-up)
# - Pin 1 (D1 / GP1): D-pad DOWN (Active LOW, internal pull-up)
# - Pin 2 (D2 / GP2): D-pad LEFT (Active LOW, internal pull-up)
# - Pin 3 (D3 / GP3): D-pad RIGHT (Active LOW, internal pull-up)
# - Pin 4 (D4 / GP4): D-pad UP (Active LOW, internal pull-up)
# - Pin 5 (D5 / GP5): L and R buttons together (Active LOW, internal pull-up)
#
# Hardware Connections:
# - Connect each switch/button between its designated GPIO pin and GND.
# - Onboard LED: Illuminates when any controller input is actively asserted.

import struct
import time
import board
import digitalio
import usb_hid
from common import get_pin, get_led_pin

# Pin Configuration Constants (resolves dynamically across Pico GPx and ESP32 IOx)
PIN_BTN_A: board.Pin = get_pin(0)
PIN_DPAD_DOWN: board.Pin = get_pin(1)
PIN_DPAD_LEFT: board.Pin = get_pin(2)
PIN_DPAD_RIGHT: board.Pin = get_pin(3)
PIN_DPAD_UP: board.Pin = get_pin(4)
PIN_BTN_LR: board.Pin = get_pin(5)

# Nintendo Switch HID Constants
HID_USAGE_PAGE_GENERIC: int = 0x01
HID_USAGE_GAMEPAD: int = 0x05

# Button Bitmasks (16-bit uint16 little-endian)
BTN_NONE: int = 0x0000
BTN_Y: int = 1 << 0
BTN_B: int = 1 << 1
BTN_A: int = 1 << 2
BTN_X: int = 1 << 3
BTN_L: int = 1 << 4
BTN_R: int = 1 << 5
BTN_ZL: int = 1 << 6
BTN_ZR: int = 1 << 7
BTN_MINUS: int = 1 << 8
BTN_PLUS: int = 1 << 9
BTN_LSTICK: int = 1 << 10
BTN_RSTICK: int = 1 << 11
BTN_HOME: int = 1 << 12
BTN_CAPTURE: int = 1 << 13

# Hat Switch (D-Pad) Values
HAT_UP: int = 0x00
HAT_UP_RIGHT: int = 0x01
HAT_RIGHT: int = 0x02
HAT_DOWN_RIGHT: int = 0x03
HAT_DOWN: int = 0x04
HAT_DOWN_LEFT: int = 0x05
HAT_LEFT: int = 0x06
HAT_UP_LEFT: int = 0x07
HAT_CENTER: int = 0x08

# Analog Stick Neutral Position
STICK_CENTER: int = 128

# Debounce duration in seconds
DEBOUNCE_DELAY_SECONDS: float = 0.02


class SwitchGamepad:
    # Manages communication with the Nintendo Switch HID gamepad device endpoint.

    def __init__(self) -> None:
        self._device: usb_hid.Device | None = None
        for dev in usb_hid.devices:
            if dev.usage_page == HID_USAGE_PAGE_GENERIC and dev.usage == HID_USAGE_GAMEPAD:
                self._device = dev
                break

        if self._device is None:
            raise RuntimeError(
                "Switch Gamepad HID device not found. Ensure boot.py is installed and board was power-cycled."
            )

        self._last_report: bytes = b""

    def send_state(
        self,
        buttons: int = BTN_NONE,
        hat: int = HAT_CENTER,
        lx: int = STICK_CENTER,
        ly: int = STICK_CENTER,
        rx: int = STICK_CENTER,
        ry: int = STICK_CENTER,
    ) -> None:
        # Packs and sends the 8-byte Switch controller report if state has changed.
        report: bytes = struct.pack("<HBBBBBB", buttons, hat, lx, ly, rx, ry, 0x00)
        if report != self._last_report:
            # When inputs transition rapidly (e.g. DOWN followed quickly by UP),
            # the USB IN endpoint buffer may be busy awaiting host polling.
            # Retry with short delays to allow the previous packet to clear.
            for attempt in range(10):
                try:
                    self._device.send_report(report)
                    self._last_report = report
                    return
                except OSError:
                    time.sleep(0.005)

            # Final attempt catch to avoid crashing if USB host is disconnected/lagging
            try:
                self._device.send_report(report)
                self._last_report = report
            except OSError:
                pass


class DebouncedInput:
    # Debounces digital input transitions for active-low physical switches with pull-ups.

    def __init__(self, pin: board.Pin, name: str, debounce_delay_s: float = DEBOUNCE_DELAY_SECONDS) -> None:
        self.io: digitalio.DigitalInOut = digitalio.DigitalInOut(pin)
        self.io.direction = digitalio.Direction.INPUT
        self.io.pull = digitalio.Pull.UP
        self.name: str = name
        self.debounce_delay_s: float = debounce_delay_s

        # Active LOW: True when pin is connected to GND (switch ON / button pressed)
        self.is_active: bool = not self.io.value
        self._last_raw_value: bool = self.io.value
        self._last_change_time: float = time.monotonic()

    def update(self) -> bool:
        # Polls input state with debouncing. Returns True if debounced active state changed.
        raw_val: bool = self.io.value
        now: float = time.monotonic()

        if raw_val != self._last_raw_value:
            self._last_raw_value = raw_val
            self._last_change_time = now

        if (now - self._last_change_time) >= self.debounce_delay_s:
            debounced_active: bool = not raw_val
            if debounced_active != self.is_active:
                self.is_active = debounced_active
                return True

        return False


def compute_hat_direction(up: bool, down: bool, left: bool, right: bool) -> int:
    # Resolves directional inputs into 8-way hat switch values with opposing-axis cancellation.
    if up and down:
        up = down = False
    if left and right:
        left = right = False

    if up:
        if right:
            return HAT_UP_RIGHT
        if left:
            return HAT_UP_LEFT
        return HAT_UP
    elif down:
        if right:
            return HAT_DOWN_RIGHT
        if left:
            return HAT_DOWN_LEFT
        return HAT_DOWN
    elif right:
        return HAT_RIGHT
    elif left:
        return HAT_LEFT
    else:
        return HAT_CENTER


def main() -> None:
    # Initializes peripherals, monitors GPIO inputs, and dispatches Switch controller reports.
    print("Initializing Nintendo Switch Controller Emulator...")

    gamepad: SwitchGamepad = SwitchGamepad()

    # Initialize debounced GPIO inputs with display names
    inputs: list[DebouncedInput] = [
        DebouncedInput(PIN_BTN_A, "A"),
        DebouncedInput(PIN_DPAD_DOWN, "D-pad DOWN"),
        DebouncedInput(PIN_DPAD_LEFT, "D-pad LEFT"),
        DebouncedInput(PIN_DPAD_RIGHT, "D-pad RIGHT"),
        DebouncedInput(PIN_DPAD_UP, "D-pad UP"),
        DebouncedInput(PIN_BTN_LR, "L+R"),
    ]
    switch_a, switch_dpad_down, switch_dpad_left, switch_dpad_right, switch_dpad_up, switch_lr = inputs

    # Initialize onboard LED indicator if available
    led: digitalio.DigitalInOut | None = None
    led_pin: board.Pin | None = get_led_pin()
    if led_pin is not None:
        try:
            led = digitalio.DigitalInOut(led_pin)
            led.direction = digitalio.Direction.OUTPUT
            led.value = False
        except Exception:
            pass

    print("Switch Controller Emulator ready.")
    print("  - Pin 0 (D0 / GP0): A")
    print("  - Pin 1 (D1 / GP1): D-pad DOWN")
    print("  - Pin 2 (D2 / GP2): D-pad LEFT")
    print("  - Pin 3 (D3 / GP3): D-pad RIGHT")
    print("  - Pin 4 (D4 / GP4): D-pad UP")
    print("  - Pin 5 (D5 / GP5): L+R")

    # Send initial neutral state
    gamepad.send_state(buttons=BTN_NONE, hat=HAT_CENTER)

    while True:
        state_changed: bool = False

        # Poll all inputs and log state transitions
        for inp in inputs:
            if inp.update():
                state_changed = True
                status_str: str = "DOWN" if inp.is_active else "UP"
                print(f"{inp.name}: {status_str}")

        if state_changed:
            buttons: int = BTN_NONE

            # Button evaluation
            if switch_a.is_active:
                buttons |= BTN_A
            if switch_lr.is_active:
                buttons |= (BTN_L | BTN_R)

            # D-pad hat direction evaluation
            hat: int = compute_hat_direction(
                up=switch_dpad_up.is_active,
                down=switch_dpad_down.is_active,
                left=switch_dpad_left.is_active,
                right=switch_dpad_right.is_active,
            )

            gamepad.send_state(buttons=buttons, hat=hat)

            # Update LED indicator
            if led is not None:
                led.value = (buttons != BTN_NONE or hat != HAT_CENTER)

        # Short sleep to yield execution while keeping low input latency
        time.sleep(0.002)


if __name__ == "__main__":
    main()
