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

## Powering the Pico

### How to Connect External USB Power (Battery Pack)

When powering the Pico from a stripped USB cable or battery pack:

```text
       [ USB PORT ]
 (Pin 1)  [        ]  (Pin 40) VBUS
 (Pin 2)  [  PICO  ]  (Pin 39) VSYS  <--- 🔴 RED (+5V) Wire
 (Pin 3)  [        ]  (Pin 38) GND   <--- ⚫ BLACK (GND) Wire
```

- **🔴 Red Cable (+5V)** $\rightarrow$ **Pin 39 (`VSYS`)**
  - Connect to `VSYS` (Pin 39) rather than `VBUS` (Pin 40). `VSYS` feeds the onboard 3.3V switching regulator directly.
  - The Pico has an onboard Schottky diode between the onboard USB port and `VSYS`, so powering via `VSYS` prevents power from backfeeding into your computer if you plug in the PC USB cable.
- **⚫ Black Cable (GND)** $\rightarrow$ **Pin 38 (`GND`)** (or any other GND pin).
  - *Note on 5-wire USB cables*: If the cable has two black wires, one is Signal GND (Pin 4) and the other is Shield GND (outer metal housing). Both can be connected to Pico GND.


