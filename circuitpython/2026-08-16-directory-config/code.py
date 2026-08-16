#!/usr/bin/env circuit-run
# code.py
# Main loop that blinks the onboard LED using imported settings from settings_data.py.
import board
import digitalio
import time

#file:settings.html

# Default fallback settings class
class DefaultSettings:
    device_name: str = "Pico Default (Fallback)"
    blink_rate: float = 0.5
    feature_enabled: bool = True
    key_combo: str = "ctrl+a"

settings: DefaultSettings

try:
    import settings_data  # type: ignore
    settings = settings_data  # type: ignore
except ImportError:
    settings = DefaultSettings()

# Initialize LED
led: digitalio.DigitalInOut = digitalio.DigitalInOut(board.LED)
led.direction = digitalio.Direction.OUTPUT

print("----- Pico Configuration Started -----")
print(f"Device Name: {settings.device_name}")
print(f"Blink Rate: {settings.blink_rate}s")
print(f"Blinking Enabled: {settings.feature_enabled}")
print(f"Key Combination: {getattr(settings, 'key_combo', 'ctrl+a')}")
print("--------------------------------------")

last_blink_time: float = time.monotonic()
led_state: bool = False

while True:
    current_time: float = time.monotonic()
    
    # Retrieve attributes with defaults in case of incomplete custom settings
    blink_rate: float = getattr(settings, "blink_rate", 0.5)
    feature_enabled: bool = getattr(settings, "feature_enabled", True)
    
    if feature_enabled and blink_rate > 0:
        if current_time - last_blink_time >= blink_rate:
            led_state = not led_state
            led.value = led_state
            last_blink_time = current_time
    else:
        led.value = False
        
    time.sleep(0.01)
