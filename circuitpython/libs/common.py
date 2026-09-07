# Common helper utilities for CircuitPython projects across microcontroller platforms (Pico, ESP32, etc.)

import board
import busio

# Board pin-naming families returned by get_board_family().
BOARD_RP2040: str = "rp2040"
BOARD_ESP32: str = "esp32"


def get_board_family() -> str:
    """
    Returns the pin-naming family of the running board: BOARD_RP2040 for the
    Raspberry Pi Pico family, BOARD_ESP32 for ESP32 boards.

    Scripts that run on both families use this to choose their own GPIO
    assignments. A GPIO number identifies a different physical pin on each
    family, so a single number cannot serve both.
    """
    return BOARD_RP2040 if hasattr(board, "GP0") else BOARD_ESP32


def get_pin(gpio: int) -> board.Pin:
    """
    Resolves an SoC GPIO number to the board pin object exposing it.

    A number always means the SoC GPIO number - the identifier shared by the
    datasheet, microcontroller.pin, ESP-IDF, config.toml, and the silkscreen on
    the Pico (GP#) and the ESP32-S3-DevKitC-1 (bare numbers). It is never a
    silkscreen 'D' index: on the Seeed XIAO those count header positions rather
    than GPIOs (silk D0 is GPIO1), so honouring them here would leave a bare
    number meaning two different pins on that board.

    Only the board module is consulted, never microcontroller.pin, because only
    the board definition knows which GPIOs the vendor actually broke out. A GPIO
    that exists on the die but reaches no header pad must raise rather than hand
    back a pin nothing can be wired to.

    Args:
        gpio: SoC GPIO number, e.g. 5 for GPIO5 (silk GP5 on Pico, IO5 on ESP32).

    Returns:
        The board.Pin exposing that GPIO.

    Raises:
        ValueError: If this board does not break out that GPIO.
    """
    for candidate in (f"GP{gpio}", f"IO{gpio}"):
        if hasattr(board, candidate):
            return getattr(board, candidate)
    raise ValueError(f"GPIO{gpio} is not broken out on this board")


def get_led_pin() -> board.Pin | None:
    """
    Returns the onboard LED pin if available on the current board, or None.

    Looked up by name rather than by GPIO number because on Pico W / Pico 2 W the
    LED hangs off the CYW43 wireless module instead of an RP2040 pad, so it has
    no GPIO number at all and board.LED is the only handle that reaches it.
    """
    for attr in ("LED", "LED_RED", "LED_BLUE", "LED_GREEN", "USER_LED"):
        if hasattr(board, attr):
            return getattr(board, attr)
    return None


def get_i2c(scl: int | None = None, sda: int | None = None) -> busio.I2C:
    """
    Returns an initialized busio.I2C bus.

    Args:
        scl: SoC GPIO number for the clock line, or None to use the board default.
        sda: SoC GPIO number for the data line, or None to use the board default.
    """
    if scl is not None and sda is not None:
        return busio.I2C(scl=get_pin(scl), sda=get_pin(sda))

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

    # 3. Fallback (scl, sda) GPIO pairs for boards exposing no default SCL/SDA
    # attributes (e.g. the standard Pico). Listed per family because the same
    # GPIO numbers belong to different peripherals on each.
    if get_board_family() == BOARD_RP2040:
        fallback_pairs = (
            (11, 10),  # RP2040 I2C1 (silk GP11 / GP10)
            (3, 2),    # RP2040 I2C1 (silk GP3 / GP2)
            (5, 4),    # RP2040 I2C0 (silk GP5 / GP4)
            (1, 0),    # RP2040 I2C0 (silk GP1 / GP0)
        )
    else:
        fallback_pairs = (
            (6, 5),  # ESP32-S3 default (XIAO silk D5 / D4)
            (9, 8),  # ESP32-S3 alternative (XIAO silk D10 / D9)
        )

    for fallback_scl, fallback_sda in fallback_pairs:
        try:
            return busio.I2C(scl=get_pin(fallback_scl), sda=get_pin(fallback_sda))
        except Exception:
            continue

    raise RuntimeError("Could not initialize a valid I2C bus on this board")
