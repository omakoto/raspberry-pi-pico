#!/usr/bin/env circuit-run
#file: ../libs/common.py
#
# Passive Buzzer and Push Button Firmware for CircuitPython (ESP32, Raspberry Pi Pico, and compatible boards).
#
# Sounds a passive buzzer at 880 Hz using PWM while the push button is held down.
#
# Default Pin Connections (SoC GPIO numbers):
# - GPIO 1: Push Button -> Connected between GPIO 1 and GND (active LOW, internal pull-up)
# - GPIO 2: Passive Buzzer -> Positive (+) / signal pin to GPIO 2, negative (-) pin to GND
# - Common Ground: Board GND pin

import time
import board
import digitalio
import pwmio
from common import get_led_pin, get_pin

# Pin Configuration Constants (SoC GPIO numbers: e.g. IO1/IO2 on ESP32-S3, GP1/GP2 on Pico)
GPIO_BUTTON: int = 1
GPIO_BUZZER: int = 2

# Buzzer Audio Configuration
BUZZER_FREQUENCY_HZ: int = 880
BUZZER_DUTY_CYCLE_ON: int = 32768  # 50% duty cycle (32768 / 65535) for maximum passive buzzer volume
BUZZER_DUTY_CYCLE_OFF: int = 0

# Button Debounce Configuration
DEBOUNCE_DELAY_SECONDS: float = 0.02

# Resolved Board Pins
PIN_BUTTON: board.Pin = get_pin(GPIO_BUTTON)
PIN_BUZZER: board.Pin = get_pin(GPIO_BUZZER)


# Debounces digital input transitions for an active-LOW physical button with internal pull-up.
class DebouncedButton:

    def __init__(self, pin: board.Pin, debounce_delay_s: float = DEBOUNCE_DELAY_SECONDS) -> None:
        self.io: digitalio.DigitalInOut = digitalio.DigitalInOut(pin)
        self.io.direction = digitalio.Direction.INPUT
        self.io.pull = digitalio.Pull.UP
        self.debounce_delay_s: float = debounce_delay_s

        # Active LOW: False when button is pressed (connected to GND), True when released
        self.is_pressed: bool = not self.io.value
        self._last_raw_value: bool = self.io.value
        self._last_change_time: float = time.monotonic()

    def update(self) -> bool:
        # Polls the button state with time-based debouncing.
        # Returns True if the debounced pressed state changed, False otherwise.
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


def init_led() -> digitalio.DigitalInOut | None:
    # Initializes the onboard status LED if available on the current board.
    led_pin: board.Pin | None = get_led_pin()
    if led_pin is None:
        return None
    try:
        led: digitalio.DigitalInOut = digitalio.DigitalInOut(led_pin)
        led.direction = digitalio.Direction.OUTPUT
        led.value = False
        return led
    except Exception:
        return None


def main() -> None:
    # Initializes button, passive buzzer PWM output, and runs the main polling loop.
    print("Initializing Passive Buzzer Test...")
    print(f"  Button Pin : GPIO{GPIO_BUTTON} (active LOW, internal pull-up)")
    print(f"  Buzzer Pin : GPIO{GPIO_BUZZER} (PWM at {BUZZER_FREQUENCY_HZ} Hz)")

    button: DebouncedButton = DebouncedButton(PIN_BUTTON)
    buzzer: pwmio.PWMOut = pwmio.PWMOut(
        PIN_BUZZER,
        frequency=BUZZER_FREQUENCY_HZ,
        duty_cycle=BUZZER_DUTY_CYCLE_ON if button.is_pressed else BUZZER_DUTY_CYCLE_OFF,
    )
    led: digitalio.DigitalInOut | None = init_led()
    if led is not None:
        led.value = button.is_pressed

    print("System ready. Hold the button to sound the buzzer at 880 Hz.")

    while True:
        if button.update():
            if button.is_pressed:
                buzzer.duty_cycle = BUZZER_DUTY_CYCLE_ON
                if led is not None:
                    led.value = True
                print(f"Button PRESSED (GPIO{GPIO_BUTTON}) -> Buzzer ON ({BUZZER_FREQUENCY_HZ} Hz)")
            else:
                buzzer.duty_cycle = BUZZER_DUTY_CYCLE_OFF
                if led is not None:
                    led.value = False
                print(f"Button RELEASED (GPIO{GPIO_BUTTON}) -> Buzzer OFF")

        time.sleep(0.002)


if __name__ == "__main__":
    main()
