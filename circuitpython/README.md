# CircuitPython Development

This directory contains CircuitPython projects, scripts, and helper utilities for Raspberry Pi Pico development.

*Note: Unless specified otherwise, the default target hardware for scripts in this directory is the Raspberry Pi Pico 2 W.*

## How monitor stdout

```bash
picocom -b 115200 /dev/ttyACM0
# ctrl-a q to quit
```

## Code Conventions

- **Shebang Line & Executable Permission**: Every main CircuitPython script should start with the shebang:
  ```python
  #!/usr/bin/env circuit-run
  ```
  Always make Python scripts executable so that they can run directly:
  ```bash
  chmod +x <script-name>.py
  ```

- **Pin Configuration Constants**: Define all the GPIO pin number / etc at the top of the script for easy customization.

- **Comments vs Docstrings**: Avoid using Python triple-quoted docstrings (`"""..."""`) as CircuitPython may encounter syntax or memory constraints when parsing them; use standard `#` comments instead.

## Project Directories

Below is an overview of the projects and utilities contained in this directory:

### Projects & Libraries

| Project | Description |
| :--- | :--- |
| [`2026-08-15-first-led`](file:///home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/2026-08-15-first-led) | A simple test script ([`led-blinker.py`](file:///home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/2026-08-15-first-led/led-blinker.py)) to blink the onboard LED. |
| [`2026-08-15-rotary-test-ec11`](file:///home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/2026-08-15-rotary-test-ec11) | Reads and prints rotation steps/directions from an EC11 rotary encoder ([`rotary-encoder.py`](file:///home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/2026-08-15-rotary-test-ec11/rotary-encoder.py)). |
| [`2026-08-15-sht31-test`](file:///home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/2026-08-15-sht31-test) | Reads and prints temperature/humidity values from an SHT31 sensor over I2C ([`sht31-test.py`](file:///home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/2026-08-15-sht31-test/sht31-test.py)). |
| [`2026-08-15-ssd1306-test`](file:///home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/2026-08-15-ssd1306-test) | Initializes and renders simple text on an SSD1306 OLED display using standard I2C connection ([`ssd1306-test.py`](file:///home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/2026-08-15-ssd1306-test/ssd1306-test.py)). |
| [`2026-08-16-ir-receiver`](file:///home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/2026-08-16-ir-receiver) | Captures raw IR pulses and logs NEC remote protocol commands ([`ir-receiver.py`](file:///home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/2026-08-16-ir-receiver/ir-receiver.py)). |
| [`2026-08-16-ir-cloner`](file:///home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/2026-08-16-ir-cloner) | Recording cloner and blaster that logs remote signals and replicates them on an IR LED transmitter ([`ir-cloner.py`](file:///home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/2026-08-16-ir-cloner/ir-cloner.py)). |
| [`2026-08-16-usb-keyboard`](file:///home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/2026-08-16-usb-keyboard) | Emulates a USB HID keyboard, sending keys in response to physical buttons ([`usb-keyboard.py`](file:///home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/2026-08-16-usb-keyboard/usb-keyboard.py)). |
| [`2026-08-16-web-serial-config`](file:///home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/2026-08-16-web-serial-config) | Configuration settings portal running over secondary USB Serial (CDC Data), parsing/saving options to JSON with storage protection selector GP14. |
| [`2026-08-16-directory-config`](file:///home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/2026-08-16-directory-config) | Zero-connection settings portal using browser Directory Picker API to write config values directly as variables to `settings_data.py`, triggering soft-reboot on write. |
| [`2026-08-16-ifttt-desk-light`](file:///home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/2026-08-16-ifttt-desk-light) | Triggers an IFTTT webhook event via HTTP GET request when a button on GP14 is pressed ([`ifttt-desk-light.py`](file:///home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/2026-08-16-ifttt-desk-light/ifttt-desk-light.py)). |
| [`2026-08-29-gpio-monitor`](file:///home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/2026-08-29-gpio-monitor) | Dynamically monitors all available GPIO pins (0–44) across Pico/ESP32 boards with debouncing, logging on/off switch state transitions ([`gpio-monitor.py`](file:///home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/2026-08-29-gpio-monitor/gpio-monitor.py)). |
| [`ssd1306`](file:///home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/ssd1306) | SSD1306 OLED driver and terminal simulator library ([`ssd1306.py`](file:///home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/ssd1306/ssd1306.py)) supporting dynamic fonts (4x5, 8x16), ANSI sequences, and Unicode box drawing, with sample demos ([`sample.py`](file:///home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/ssd1306/sample.py), [`sample-large.py`](file:///home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/ssd1306/sample-large.py), [`sample-borders.py`](file:///home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/ssd1306/sample-borders.py), [`sample-ascii.py`](file:///home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/ssd1306/sample-ascii.py)). |

### Utilities

* [`bin/`](file:///home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/bin): Contains helper scripts (`circuit-run`, `find-circut-dir`) to run and monitor Python code on connected Pico devices, along with bash unit test files.
