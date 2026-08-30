# EC11 Rotary Encoder Test

A CircuitPython project to read and decode input events from a 5-pin EC11 rotary encoder (rotation steps and push button) on Raspberry Pi Pico, ESP32, and compatible boards.

Uses a software-based quadrature state-table decoder, enabling support for arbitrary, non-sequential GPIO pins across different microcontrollers via [`libs/common.py`](file:///home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/libs/common.py).

---

## Encoder Pinout Diagram

Top view of the 5-pin rotary encoder (shaft facing up):

```text
                  Top View (Shaft Facing Up)

                +---------------------------+
  Encoder A --> | ( A )    ( C )    ( B ) | <-- Encoder B
                |            |              |
                |           GND             |
                |                           |
                |         /-------\         |
                |        (  Shaft  )        |
                |         \-------/         |
                |                           |
     Button --> | ( D )             ( E ) | <-- GND
                +---------------------------+
                  [ 3-pin side: Encoder ]
                  [ 2-pin side: Switch  ]
```

---

## Hardware Wiring

| Encoder Pin | Label | Function | Board Pin (Pico / ESP32) | Description |
| :--- | :--- | :--- | :--- | :--- |
| **A** | Encoder A | Phase A | **GPIO 1** (e.g. `GP1` / `IO1`) | Quadrature Channel A |
| **C** | GND | Common Ground | **GND** | Encoder ground reference |
| **B** | Encoder B | Phase B | **GPIO 2** (e.g. `GP2` / `IO2`) | Quadrature Channel B |
| **D** | Button | Push Switch | **GPIO 3** (e.g. `GP3` / `IO3`) | Active LOW (internal pull-up enabled) |
| **E** | GND | Switch Ground | **GND** | Push button ground reference |

> *Note: Internal pull-up resistors (`Pull.UP`) are enabled in software for GPIO 1, 2, and 3, so no external pull-up resistors are required.*

---

## How It Works

1. **Quadrature Decoding**:
   - The encoder produces 2-bit Gray code output on Phase A and Phase B.
   - A 16-element transition lookup table (`0b[last_a][last_b][a][b]`) tracks state changes, filters invalid transitions, and increments/decrements position counters without missing steps.
   - Detents typically complete 4 quadrature phase transitions per click (`encoder.position // 4`).

2. **Push Button**:
   - The built-in tactile momentary switch connects Pin D to Pin E (GND) when pressed.
   - Debounced state change detection prints `PRESSED` (LOW) and `RELEASED` (HIGH).

---

## How to Run

1. Run the script directly using `circuit-run`:
   ```bash
   /home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/2026-08-15-rotary-test-ec11/rotary-encoder.py
   ```
   Or copy to your `CIRCUITPY` drive as `code.py`:
   ```bash
   cp /home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/2026-08-15-rotary-test-ec11/rotary-encoder.py /run/media/$USER/CIRCUITPY/code.py
   ```

2. Open the serial console (e.g. `picocom -b 115200 /dev/ttyACM0`) to monitor rotary position steps and button click events:
   ```text
   EC11 Software-based Rotary Encoder Test Initialized.
   Rotate the encoder knob or press the button to view input events...
   Encoder Position: 1 (Raw: 4, Change: +1)
   Encoder Position: 2 (Raw: 8, Change: +1)
   Button State: PRESSED
   Button State: RELEASED
   ```
