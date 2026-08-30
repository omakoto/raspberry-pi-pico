# Common helper utilities for CircuitPython projects across microcontroller platforms (Pico, ESP32, etc.)

import board
import busio
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

    raise ValueError(f"GPIO pin {pin_id} not found on this board")


def get_led_pin() -> board.Pin | None:
    """
    Returns the onboard LED pin if available on the current board, or None.
    """
    for attr in ("LED", "LED_RED", "LED_BLUE", "LED_GREEN", "USER_LED"):
        if hasattr(board, attr):
            return getattr(board, attr)
    return None


def get_i2c(scl: int | str | None = None, sda: int | str | None = None) -> busio.I2C:
    """
    Returns an initialized busio.I2C bus.
    If scl and sda are provided, initializes I2C using those pins.
    If either is None, attempts to use the board's default I2C bus (board.I2C() or board.SCL/SDA),
    falling back to standard Pico/ESP32 default pin pairings.
    """
    if scl is not None and sda is not None:
        scl_pin = get_pin(scl)
        sda_pin = get_pin(sda)
        return busio.I2C(scl=scl_pin, sda=sda_pin)

    # 1. Try board.I2C() singleton helper
    if hasattr(board, "I2C"):
        try:
            return board.I2C()
        except Exception:
            pass

    # 2. Try default board.SCL and board.SDA pins
    if hasattr(board, "SCL") and hasattr(board, "SDA"):
        try:
            return busio.I2C(scl=board.SCL, sda=board.SDA)
        except Exception:
            pass

    # 3. Fallback candidates for boards without defined default SCL/SDA attributes (e.g. standard Pico RP2040)
    fallback_pairs = (
        ("GP11", "GP10"),  # RP2040 I2C1
        ("GP3", "GP2"),    # RP2040 I2C1
        ("GP5", "GP4"),    # RP2040 I2C0
        ("GP1", "GP0"),    # RP2040 I2C0
        ("IO6", "IO5"),    # ESP32-S3 default (Seeed XIAO D5/D4)
        ("IO9", "IO8"),    # ESP32-S3 alternative
    )
    for fallback_scl, fallback_sda in fallback_pairs:
        try:
            scl_pin = get_pin(fallback_scl)
            sda_pin = get_pin(fallback_sda)
            return busio.I2C(scl=scl_pin, sda=sda_pin)
        except Exception:
            continue

    raise RuntimeError("Could not initialize a valid I2C bus on this board")
