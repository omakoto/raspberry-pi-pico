# Nintendo Switch Controller Emulator Test (Composite Device Mode)

A CircuitPython project for Raspberry Pi Pico (Pico / Pico 2 / Pico 2 W) and ESP32-S3 that emulates a Nintendo Switch USB controller (HORI Pokkén Controller profile) using a composite USB device setup.

## Hardware Wiring

| Pin Index | Xiao Label | Pico Label | Target Action | Description |
| :---: | :---: | :---: | :--- | :--- |
| **0** | **D0** (`IO1`) | **GP0** | **A** button | Active LOW (internal pull-up). When connected to GND, sends `A` button down. |
| **1** | **D1** (`IO2`) | **GP1** | **D-pad DOWN** | Active LOW (internal pull-up). When connected to GND, sends D-pad Down. |
| **2** | **D2** (`IO3`) | **GP2** | **D-pad LEFT** | Active LOW (internal pull-up). When connected to GND, sends D-pad Left. |
| **3** | **D3** (`IO4`) | **GP3** | **D-pad RIGHT** | Active LOW (internal pull-up). When connected to GND, sends D-pad Right. |
| **4** | **D4** (`IO5`) | **GP4** | **D-pad UP** | Active LOW (internal pull-up). When connected to GND, sends D-pad Up. |
| **5** | **D5** (`IO6`) | **GP5** | **L + R** buttons | Active LOW (internal pull-up). When connected to GND, sends `L + R` held down together. |
| — | **GND** | **GND** | Common Ground | Reference connected to the common side of switches/buttons. |
| — | **LED** | **LED** | Activity Indicator | Lights up whenever any controller input is active. |

> **Note**: All inputs use internal microcontroller pull-up resistors (`digitalio.Pull.UP`), resolved dynamically via `common.get_pin(0..5)` across Pico and ESP32 boards. Simply wire each switch directly between its designated pin and GND.

---

## How It Works

### 1. Composite Device Mode (`boot.py`)
At boot time before the USB stack initializes, `boot.py`:
- Sets the USB Vendor & Product IDs to match the HORI Pokkén Controller (`VID: 0x0F0D`, `PID: 0x0092`).
- Registers the custom 8-byte Nintendo Switch Gamepad HID report descriptor (`Usage Page 0x01`, `Usage 0x05`).
- Keeps the standard CircuitPython **Mass Storage (`CIRCUITPY`)** and **USB CDC Serial (REPL)** endpoints active simultaneously.
- When plugged into a Nintendo Switch, the console reads the Gamepad HID endpoint and ignores storage/serial.
- When plugged into a PC, the flash drive mounts and the serial console remains accessible for debugging.

### 2. Controller Input Loop (`switch-controller-test.py`)
- Polls GPIOs 1–6 with software debouncing.
- Assembles and transmits the 8-byte Switch HID state frame:
  - **GPIO 1 Low (ON)**: Bitwise ORs `BTN_A (0x0004)` into the button mask.
  - **GPIO 2–5 Low (ON)**: Encodes D-pad directional hat value (supporting orthogonal and diagonal directions, with opposing-axis cancellation).
  - **GPIO 6 Low (ON)**: Bitwise ORs `BTN_L (0x0010)` and `BTN_R (0x0020)` into the button mask.
  - Dispatches reports on state transitions to minimize USB bus traffic.

---

## How to Install and Run

1. Connect your Raspberry Pi Pico running CircuitPython to your computer.
2. Copy `boot.py` to the root of the `CIRCUITPY` drive:
   ```bash
   cp boot.py /run/media/$USER/CIRCUITPY/boot.py
   ```
3. Copy `switch-controller-test.py` as `code.py` to the `CIRCUITPY` drive:
   ```bash
   cp switch-controller-test.py /run/media/$USER/CIRCUITPY/code.py
   ```
4. **Hard reset / replug the board**: A USB bus re-enumeration is required whenever `boot.py` changes USB descriptors.
5. Plug the Pico into your Nintendo Switch dock (or a PC game controller tester) and toggle GP1 and GP2.
