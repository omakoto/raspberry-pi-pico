# CircuitPython Development

This directory contains CircuitPython projects, scripts, and helper utilities for Raspberry Pi Pico development.

*Note: Unless specified otherwise, the default target hardware for scripts in this directory is the Raspberry Pi Pico 2 W.*


## Micropython vs CircuitPython

https://share.gemini.google/NTpzKBwuuLy2

## Firmware download

- https://circuitpython.org/board/raspberry_pi_pico/
- https://circuitpython.org/board/raspberry_pi_pico2_w/


## How to run code

- Just save code in `/run/media/omakoto/CIRCUITPY/code.py`. e.g.

```bash
cat >/run/media/omakoto/CIRCUITPY/code.py <<'__EOF__'
import time
import board
import digitalio

# Setup the onboard LED
led = digitalio.DigitalInOut(board.LED)
led.direction = digitalio.Direction.OUTPUT

while True:
    led.value = True
    time.sleep(0.5)
    led.value = False
    time.sleep(0.5)
    print("OK")
__EOF__
```

## How monitor stdout

```bash
picocom -b 115200 /dev/ttyACM0
# ctrl-a q to quit
```

## Code Conventions

- **Shebang Line**: Every main CircuitPython script should start with the shebang:
  ```python
  #!/usr/bin/env circuit-run
  ```
- **Executable Permission**: Always mark the script as executable so that it can run directly:
  ```bash
  chmod +x <script-name>.py
  ```
