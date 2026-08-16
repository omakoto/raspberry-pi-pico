# Raspberry Pi Pico IR Receiver & Pulse Logger

A CircuitPython project that captures raw infrared (IR) pulses and decodes remote control signals using the Raspberry Pi Pico (RP2040 / RP2350).

## Hardware Wiring

### Identifying Module Pins

Depending on whether you have a **bare 3-pin sensor component** (e.g. VS1838B / TSOP38238) or a **3-pin breakout PCB module**:

#### Option A: Breakout PCB Module (with 3 header pins)
| PCB Label | Pin Name | Pico Pin | Pico Physical Pin |
| :--- | :--- | :--- | :--- |
| **`S`** / **`DAT`** / **`OUT`** | Data / Signal Output | **GP19** | **Pin 25** |
| **`+`** / **`VCC`** | Power (3.3V) | **3V3 OUT** | **Pin 36** |
| **`-`** / **`GND`** | Ground | **GND** | **Pin 23** or **Pin 28** |

#### Option B: Bare 3-Pin Sensor Component (e.g. VS1838B / TL1838 / TSOP38238)
Hold the sensor with the **dome / rounded lens facing you** and the pins pointing downward:
```
    .-'''''-.
   |  ( O )  |  <- IR Receiver Dome facing YOU
   |_________|
     |  |  |
     1  2  3
```
- **Pin 1 (Left)**: **DATA / OUT** -> Connect to **GP19** (Pin 25)
- **Pin 2 (Middle)**: **GND** -> Connect to **GND** (Pin 23 or 28)
- **Pin 3 (Right)**: **VCC** -> Connect to **3V3 OUT** (Pin 36)

---

## Features

- **Universal Pulse Logging**: Captures and displays microsecond-level pulse/space durations for any remote control (NEC, Sony, RC-5, Samsung, etc.).
- **Automatic Protocol Decoding**:
  - **NEC Protocol**: Parses 32-bit NEC packets with Address, Command, and known button names.
  - **Sony SIRC Protocol**: Parses 12-bit, 15-bit, and 20-bit Sony remote codes with Device ID and Command names (e.g. TV keypad, volume, channels).
- **Built-in `pulseio`**: Uses CircuitPython's built-in `pulseio.PulseIn` with zero external library dependencies.
- **Visual Feedback**: Flashes the Pico's onboard LED upon receiving IR bursts.

---

## How to Run

1. Connect the Pico to your computer.
2. Run the script directly:
   ```bash
   /home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/2026-08-16-ir-receiver/ir-receiver.py
   ```
   Or copy to your CIRCUITPY drive:
   ```bash
   cp /home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/2026-08-16-ir-receiver/ir-receiver.py /run/media/omakoto/CIRCUITPY/code.py
   ```
3. Open a serial monitor (`picocom -b 115200 /dev/ttyACM0`), aim the remote at the receiver, and press any button.

---

## About IR Signals

When pressing buttons on an IR remote, you may notice varying pulse counts or lengths even for the same button. This is expected behavior in infrared protocols:

1. **Initial Press vs. Hold (Repeat Codes)**:
   - **First Press (Full Data Frame)**: Sends the complete packet (around 67 transitions in the standard NEC protocol, consisting of the 9ms leader pulse, 4.5ms space, 32 address and command data bits, and a stop pulse).
   - **Holding the Button (Repeat Frame)**: When a button is held down, most remotes avoid resending the full 32 data bits to conserve power and bandwidth. Instead, they emit short **Repeat Frames** (around 4 transitions: 9ms pulse + 2.25ms space) every ~110ms until released.

2. **Capture Window & Slicing**:
   - The receiver gathers incoming pulses within a sampling window. If a button is held down or released mid-cycle, subsequent repeat pulses or split frames may appear as separate, smaller captures in the log.

3. **Optical Noise & Jitter**:
   - Infrared receivers use Automatic Gain Control (AGC). Ambient lighting (fluorescent fixtures, screen backlights), reflections, or low remote batteries can occasionally introduce slight microsecond timing variations or 1–2 stray edge transitions at the boundary of a transmission burst.

