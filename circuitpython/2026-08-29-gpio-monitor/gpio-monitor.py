#!/usr/bin/env circuit-run
# GPIO State Change Monitor for CircuitPython.
#
# Monitors all available GPIO pins for state changes (connected to GND = ON, open = OFF).
# Scans pins dynamically using both microcontroller.pin inspection and explicit range 0-44 scanning,
# prints the discovered pins from each method at initialization, and monitors the union set with debouncing.
#
# Supported targets: Raspberry Pi Pico (RP2040 / RP2350), ESP32, and compatible boards.

import time
import board
import microcontroller
import digitalio

# Configuration constants
SCAN_MIN_PIN: int = 0
SCAN_MAX_PIN: int = 44
DEBOUNCE_INTERVAL_SECONDS: float = 0.02


def extract_pin_number(name: str):
    # Extracts the first integer digits found in a pin name string (e.g., 'GP15' -> 15, 'GPIO29' -> 29)
    digits = "".join([c for c in name if c.isdigit()])
    if digits:
        return int(digits)
    return None


def test_pin_as_input(pin_obj) -> bool:
    # Tests whether a pin object can be safely configured as a digital input with pull-up.
    # Immediately deinits the pin so it can be allocated cleanly later.
    try:
        dio = digitalio.DigitalInOut(pin_obj)
        dio.direction = digitalio.Direction.INPUT
        dio.pull = digitalio.Pull.UP
        _ = dio.value
        dio.deinit()
        return True
    except Exception:
        return False


def discover_pins_from_microcontroller():
    # Method A: Dynamically inspects attributes in microcontroller.pin.
    # Returns a list of tuples: (pin_object, pin_name).
    discovered = []
    seen_pin_ids = set()

    for attr in dir(microcontroller.pin):
        if attr.startswith("_"):
            continue
        try:
            pin_obj = getattr(microcontroller.pin, attr)
            if id(pin_obj) in seen_pin_ids:
                continue
            if test_pin_as_input(pin_obj):
                discovered.append((pin_obj, attr))
                seen_pin_ids.add(id(pin_obj))
        except Exception:
            continue

    return discovered


def discover_pins_from_range(min_pin: int = SCAN_MIN_PIN, max_pin: int = SCAN_MAX_PIN):
    # Method B: Probes pin index range (min_pin to max_pin) looking up known naming patterns
    # on both board and microcontroller.pin modules.
    # Returns a list of tuples: (pin_object, pin_name).
    discovered = []
    seen_pin_ids = set()

    for pin_num in range(min_pin, max_pin + 1):
        candidates = [
            (board, f"GP{pin_num}"),
            (board, f"GPIO{pin_num}"),
            (board, f"IO{pin_num}"),
            (board, f"D{pin_num}"),
            (microcontroller.pin, f"GPIO{pin_num}"),
            (microcontroller.pin, f"GP{pin_num}"),
            (microcontroller.pin, f"IO{pin_num}"),
        ]

        for module_obj, attr_name in candidates:
            if hasattr(module_obj, attr_name):
                try:
                    pin_obj = getattr(module_obj, attr_name)
                    if id(pin_obj) in seen_pin_ids:
                        continue
                    if test_pin_as_input(pin_obj):
                        discovered.append((pin_obj, f"GPIO{pin_num}"))
                        seen_pin_ids.add(id(pin_obj))
                        break
                except Exception:
                    continue

    return discovered


# Manages software debouncing for a single digital input pin with pull-up.
class DebouncedInput:
    def __init__(self, pin_obj, label: str, debounce_interval_s: float = DEBOUNCE_INTERVAL_SECONDS) -> None:
        self.label: str = label
        self.debounce_interval_s: float = debounce_interval_s
        self.dio: digitalio.DigitalInOut = digitalio.DigitalInOut(pin_obj)
        self.dio.direction = digitalio.Direction.INPUT
        self.dio.pull = digitalio.Pull.UP

        # Active LOW (Connected to GND = True = ON)
        self.is_on: bool = not self.dio.value
        self._last_raw_value: bool = self.dio.value
        self._last_transition_time: float = time.monotonic()

    def update(self) -> bool:
        # Polls the pin state and updates debounced status.
        # Returns True if a state change occurred, False otherwise.
        raw_val: bool = self.dio.value
        now: float = time.monotonic()

        if raw_val != self._last_raw_value:
            self._last_raw_value = raw_val
            self._last_transition_time = now

        if (now - self._last_transition_time) >= self.debounce_interval_s:
            debounced_on: bool = not raw_val
            if debounced_on != self.is_on:
                self.is_on = debounced_on
                return True

        return False


def run_keypad_monitor(pin_list, pin_labels, debounce_s: float) -> None:
    # Runs the monitor loop using CircuitPython's hardware-accelerated keypad module.
    import keypad

    keys = keypad.Keys(
        pins=tuple(pin_list),
        value_when_pressed=False,  # Active LOW (GND = pressed = ON)
        pull=True,                 # Internal pull-up resistor
        interval=debounce_s,
        max_events=128,
    )

    event = keypad.Event()
    print("\n--- Monitoring Started (keypad module) ---")
    while True:
        if keys.events.get_into(event):
            label = pin_labels[event.key_number]
            state_str = "ON" if event.pressed else "OFF"
            print(f"{label} {state_str}")
        time.sleep(0.001)


def run_polled_monitor(pin_list, pin_labels, debounce_s: float) -> None:
    # Fallback monitor loop using polled DigitalInOut debouncing.
    inputs = []
    for pin_obj, label in zip(pin_list, pin_labels):
        try:
            inputs.append(DebouncedInput(pin_obj, label, debounce_interval_s=debounce_s))
        except Exception as err:
            print(f"Warning: Could not initialize {label}: {err}")

    print("\n--- Monitoring Started (polled digitalio fallback) ---")
    while True:
        for inp in inputs:
            if inp.update():
                state_str = "ON" if inp.is_on else "OFF"
                print(f"{inp.label} {state_str}")
        time.sleep(0.001)


def main() -> None:
    # Main discovery and monitoring entrypoint.
    print("========================================")
    print(" GPIO State Change Monitor Starting")
    print("========================================")

    # 1. Discover via Method A: microcontroller.pin
    pins_method_a = discover_pins_from_microcontroller()
    labels_a = [name for _, name in pins_method_a]
    print(f"\n[Method A] Discovered {len(pins_method_a)} pins via microcontroller.pin:")
    print("  " + (", ".join(labels_a) if labels_a else "None"))

    # 2. Discover via Method B: Range 0 to 44 scan
    pins_method_b = discover_pins_from_range(SCAN_MIN_PIN, SCAN_MAX_PIN)
    labels_b = [name for _, name in pins_method_b]
    print(f"\n[Method B] Discovered {len(pins_method_b)} pins via 0-{SCAN_MAX_PIN} range scan:")
    print("  " + (", ".join(labels_b) if labels_b else "None"))

    # 3. Build Union Set
    union_dict = {}

    for pin_obj, name in pins_method_a + pins_method_b:
        num = extract_pin_number(name)
        sort_key = num if num is not None else 999
        display_label = f"GPIO#{num}" if num is not None else f"GPIO#{name}"

        # Key by pin object ID to deduplicate identical hardware pins
        if id(pin_obj) not in union_dict:
            union_dict[id(pin_obj)] = (pin_obj, display_label, sort_key)

    # Sort union by pin number for predictable display and indexing
    sorted_union = sorted(union_dict.values(), key=lambda item: item[2])
    monitored_pins = [item[0] for item in sorted_union]
    monitored_labels = [item[1] for item in sorted_union]

    print(f"\n[Union Set] Monitoring {len(monitored_pins)} unique pins in total:")
    print("  " + (", ".join(monitored_labels) if monitored_labels else "None"))
    print("Wiring: Connect pin to GND for ON (active LOW with internal pull-up).")
    print("----------------------------------------")

    if not monitored_pins:
        print("Error: No valid digital input pins were found to monitor.")
        return

    # Attempt to use keypad module with fallback to polled debouncing
    try:
        run_keypad_monitor(monitored_pins, monitored_labels, DEBOUNCE_INTERVAL_SECONDS)
    except (ImportError, AttributeError, NotImplementedError) as err:
        print(f"Keypad module not usable ({err}). Falling back to polled digitalio.")
        run_polled_monitor(monitored_pins, monitored_labels, DEBOUNCE_INTERVAL_SECONDS)


if __name__ == "__main__":
    main()
