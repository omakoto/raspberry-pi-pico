# GPIO State Change Monitor

A CircuitPython project that dynamically monitors all available GPIO pins for on-off switch state transitions.

Whenever a switch toggles, it prints the GPIO pin identifier and its updated state (e.g. `GPIO#29 ON` or `GPIO#29 OFF`).

## Features

- **Dual-Method Dynamic Pin Discovery**:
  - **Method A (`microcontroller.pin`)**: Dynamically enumerates all pins exposed by the microcontroller hardware layer.
  - **Method B (Range scan `0` to `44`)**: Probes pin numbering `GPIO0`–`GPIO44` / `GP0`–`GP44` / `IO0`–`IO44` across `board` and `microcontroller.pin`.
  - **Union Set**: Prints the discovered pins from both methods at initialization, deduplicates identical hardware pins, and monitors the union set.
- **Safe Pin Validation**: Safely catches and skips unavailable, reserved, or non-digital pins without crashing.
- **Hardware & Software Debouncing**:
  - Uses CircuitPython's hardware-accelerated `keypad.Keys` module with non-blocking 20ms debouncing.
  - Includes an automatic fallback to polled `digitalio.DigitalInOut` debouncing if `keypad` is unavailable.
- **Active LOW Wiring**: Configures internal pull-up resistors (`Pull.UP`), so connecting a pin to **GND** triggers **ON**, and disconnecting/opening returns to **OFF**.

## Hardware Wiring

Connect any toggle or momentary switch between the desired GPIO pin and **GND**:

| Switch Pin | Board Pin | Logic State | Output |
| :--- | :--- | :--- | :--- |
| One side | **GPIOx** (e.g. GP0–GP44) | Pulled LOW (GND) | `GPIO#x ON` |
| Other side | **GND** | Open / Floating (High) | `GPIO#x OFF` |

> *Note: Internal pull-up resistors are enabled automatically, so no external pull-up resistors are required.*

## How to Run

1. Connect your Raspberry Pi Pico, ESP32, or compatible board running CircuitPython.
2. Run the script directly using `circuit-run`:
   ```bash
   ./gpio-monitor.py
   ```
   Or copy it to your `CIRCUITPY` drive as `code.py`:
   ```bash
   cp gpio-monitor.py /run/media/$USER/CIRCUITPY/code.py
   ```
3. Open the serial console (e.g. `picocom -b 115200 /dev/ttyACM0`) to view discovered pins and live state changes.
