# Random Notes on Pico Development

## Micropython vs CircuitPython

https://share.gemini.google/NTpzKBwuuLy2

## CircuitPython
### Firmware download

- https://circuitpython.org/board/raspberry_pi_pico/
- https://circuitpython.org/board/raspberry_pi_pico2_w/


### How to run code

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

