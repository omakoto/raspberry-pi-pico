#!/usr/bin/env circuit-run
# code.py
# Main loop that listens for Web Serial configuration and manages LED blink rates.

#file: boot.py
#file: settings.html

import board
import digitalio
import json
import time
import usb_cdc

# Default settings
DEFAULT_SETTINGS: dict[str, any] = {
    "device_name": "Pico Configurable Device",
    "blink_rate": 0.5,
    "feature_enabled": True
}

# Pin Definitions
PIN_STORAGE_SWITCH: board.Pin = board.GP14
PIN_LED: board.Pin = board.LED

# Determine if the filesystem is read-only for CircuitPython
# boot.py configures storage.remount based on storage switch pin.
# We can read the same pin to know our write status:
# Switch High (value True) means boot.py remounted as read-write to Pico.
# Switch Low (value False) means it is read-only to Pico.
gp14_pin: digitalio.DigitalInOut = digitalio.DigitalInOut(PIN_STORAGE_SWITCH)
gp14_pin.direction = digitalio.Direction.INPUT
gp14_pin.pull = digitalio.Pull.UP
is_readonly: bool = not gp14_pin.value

# Initialize LED
led: digitalio.DigitalInOut = digitalio.DigitalInOut(PIN_LED)
led.direction = digitalio.Direction.OUTPUT

# Load settings from settings.json
settings: dict[str, any] = DEFAULT_SETTINGS.copy()

def load_settings() -> None:
    global settings
    try:
        with open("settings.json", "r") as f:
            data: dict[str, any] = json.load(f)
            # Update settings with loaded values
            for k in DEFAULT_SETTINGS:
                if k in data:
                    settings[k] = data[k]
    except (OSError, ValueError):
        # File doesn't exist or is invalid JSON
        if not is_readonly:
            save_settings(DEFAULT_SETTINGS)

def save_settings(new_settings: dict[str, any]) -> bool:
    global settings
    if is_readonly:
        return False
    try:
        with open("settings.json", "w") as f:
            json.dump(new_settings, f)
        settings = new_settings.copy()
        return True
    except OSError:
        return False

# Initial settings load
load_settings()

# Setup Serial Data Port
serial_port: usb_cdc.SerialPort = usb_cdc.data

# LED Blink state variables
last_blink_time: float = time.monotonic()
led_state: bool = False

print("Pico Web Serial Config POC Started.")

while True:
    # 1. Update LED blinking based on settings
    current_time: float = time.monotonic()
    blink_rate: float = float(settings.get("blink_rate", 0.5))
    feature_enabled: bool = bool(settings.get("feature_enabled", True))
    
    if feature_enabled and blink_rate > 0:
        if current_time - last_blink_time >= blink_rate:
            led_state = not led_state
            led.value = led_state
            last_blink_time = current_time
    else:
        led.value = False

    # 2. Process incoming Serial messages
    if serial_port and serial_port.connected:
        if serial_port.in_waiting > 0:
            try:
                line_bytes: bytes = serial_port.readline()
                line: str = line_bytes.decode("utf-8").strip()
                if not line:
                    continue
                
                request: dict[str, any] = json.loads(line)
                command: str = request.get("command", "")
                
                if command == "get_state":
                    response: dict[str, any] = {
                        "status": "ok",
                        "readonly": is_readonly,
                        "settings": settings
                    }
                    serial_port.write((json.dumps(response) + "\n").encode("utf-8"))
                    
                elif command == "set_settings":
                    new_settings: dict[str, any] = request.get("settings", {})
                    # Clean and validate types
                    cleaned_settings: dict[str, any] = {}
                    cleaned_settings["device_name"] = str(new_settings.get("device_name", DEFAULT_SETTINGS["device_name"]))
                    
                    try:
                        cleaned_settings["blink_rate"] = float(new_settings.get("blink_rate", DEFAULT_SETTINGS["blink_rate"]))
                    except ValueError:
                        cleaned_settings["blink_rate"] = DEFAULT_SETTINGS["blink_rate"]
                        
                    cleaned_settings["feature_enabled"] = bool(new_settings.get("feature_enabled", DEFAULT_SETTINGS["feature_enabled"]))
                    
                    success: bool = save_settings(cleaned_settings)
                    if success:
                        res: dict[str, any] = {
                            "status": "ok",
                            "message": "Settings saved successfully.",
                            "settings": settings
                        }
                    else:
                        res: dict[str, any] = {
                            "status": "error",
                            "message": "Failed to write: Filesystem is read-only. Connect GP14 to GND and reboot."
                        }
                    serial_port.write((json.dumps(res) + "\n").encode("utf-8"))
            except Exception as e:
                # Catch any json decoding or other unexpected errors to prevent main loop crash
                err_res: dict[str, any] = {
                    "status": "error",
                    "message": f"Server error: {str(e)}"
                }
                try:
                    serial_port.write((json.dumps(err_res) + "\n").encode("utf-8"))
                except Exception:
                    pass

    # Small sleep to yield CPU
    time.sleep(0.01)
