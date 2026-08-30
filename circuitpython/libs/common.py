# Common helper utilities for CircuitPython projects across microcontroller platforms (Pico, ESP32, etc.)

import board
import microcontroller

def get_pin(pin_id: int | str) -> board.Pin:
    """
    Resolves a GPIO pin identifier across different board architectures (Raspberry Pi Pico, ESP32/ESP32-S3, etc.).

    Args:
        pin_id: An integer pin number (e.g. 1, 15) or pin name string (e.g. 'GP1', 'IO1', 'D1', 'GPIO1').

    Returns:
        The board.Pin object corresponding to the requested GPIO.

    Raises:
        ValueError: If the pin cannot be found on the current board.
    """
    if isinstance(pin_id, str):
        # Direct lookup by exact attribute name on board or microcontroller.pin
        if hasattr(board, pin_id):
            return getattr(board, pin_id)
        if hasattr(microcontroller.pin, pin_id):
            return getattr(microcontroller.pin, pin_id)

        # Extract numeric component if a string like 'GP1' or 'IO1' was passed on an incompatible board
        digits = "".join([c for c in pin_id if c.isdigit()])
        if digits:
            num = int(digits)
        else:
            raise ValueError(f"Invalid pin identifier: '{pin_id}'")
    else:
        num = int(pin_id)

    # Candidate prefixes across board definitions
    board_candidates = (f"GP{num}", f"IO{num}", f"D{num}", f"GPIO{num}", f"P{num}")
    for candidate in board_candidates:
        if hasattr(board, candidate):
            return getattr(board, candidate)

    # Candidate prefixes in microcontroller.pin hardware layer
    mcu_candidates = (f"GPIO{num}", f"GP{num}", f"IO{num}")
    for candidate in mcu_candidates:
        if hasattr(microcontroller.pin, candidate):
            return getattr(microcontroller.pin, candidate)

    raise ValueError(f"GPIO pin {pin_id} not found on this board (board={board.board_id})")
