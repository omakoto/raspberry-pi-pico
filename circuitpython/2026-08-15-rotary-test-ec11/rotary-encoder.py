#!/usr/bin/env circuit-run
"""
Script to test and print EC11 rotary encoder inputs on a Raspberry Pi Pico.

Pin Connections:
- GP15 (r1): Encoder Phase A
- GP9 (r2): Encoder Phase B
- GP8 (r3): Push Button (active LOW, internal pull-up)
- Common Ground: Connected directly to Pico physical GND

Note: This script uses a software-based quadrature decoder to support 
non-sequential GPIO pins on the RP2040.
"""

import board
import digitalio
import time

class SoftwareEncoder:
    """A software-based quadrature decoder for rotary encoders."""
    def __init__(self, pin_a: board.Pin, pin_b: board.Pin) -> None:
        self.pin_a: digitalio.DigitalInOut = digitalio.DigitalInOut(pin_a)
        self.pin_a.direction = digitalio.Direction.INPUT
        self.pin_a.pull = digitalio.Pull.UP

        self.pin_b: digitalio.DigitalInOut = digitalio.DigitalInOut(pin_b)
        self.pin_b.direction = digitalio.Direction.INPUT
        self.pin_b.pull = digitalio.Pull.UP

        # Initial state
        self.last_a: bool = self.pin_a.value
        self.last_b: bool = self.pin_b.value
        self.position: int = 0

        # Quadrature transition lookup table: 0b[last_a][last_b][a][b]
        self._transitions: list[int] = [
            0, -1,  1,  0,
            1,  0,  0, -1,
           -1,  0,  0,  1,
            0,  1, -1,  0
        ]

    def update(self) -> bool:
        """
        Polls the encoder pins. 
        Returns True if position changed, False otherwise.
        """
        a: bool = self.pin_a.value
        b: bool = self.pin_b.value

        if a != self.last_a or b != self.last_b:
            index: int = (int(self.last_a) << 3) | (int(self.last_b) << 2) | (int(a) << 1) | int(b)
            change: int = self._transitions[index]
            if change != 0:
                self.position += change
            self.last_a = a
            self.last_b = b
            return change != 0
        return False

# Initialize the software encoder on GP15 and GP9
encoder: SoftwareEncoder = SoftwareEncoder(board.GP15, board.GP9)

# Initialize the push button on GP8 with an internal pull-up
button: digitalio.DigitalInOut = digitalio.DigitalInOut(board.GP8)
button.direction = digitalio.Direction.INPUT
button.pull = digitalio.Pull.UP

# Keep track of the last known encoder position and button state
last_detent_position: int = encoder.position // 4
last_button_state: bool = button.value  # True means released, False means pressed

print("EC11 Software-based Rotary Encoder Test Initialized.")
print("Rotate the encoder knob or press the button to view input events...")

while True:
    # Update encoder position
    if encoder.update():
        current_detent: int = encoder.position // 4
        if current_detent != last_detent_position:
            change: int = current_detent - last_detent_position
            print(f"Encoder Position: {current_detent} (Raw: {encoder.position}, Change: {change:+d})")
            last_detent_position = current_detent

    # Read push button state
    current_button_state: bool = button.value
    if current_button_state != last_button_state:
        if not current_button_state:
            print("Button State: PRESSED")
        else:
            print("Button State: RELEASED")
        last_button_state = current_button_state

    # Small delay to debounce and avoid CPU saturation
    time.sleep(0.002)
