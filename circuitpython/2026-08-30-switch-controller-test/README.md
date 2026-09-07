# Nintendo Switch Controller Emulator Test (Composite Device Mode)

A CircuitPython project for Raspberry Pi Pico (Pico / Pico 2 / Pico 2 W) and ESP32-S3 that emulates a Nintendo Switch USB controller (HORI Pokkén Controller profile) using a composite USB device setup.

## Hardware Wiring

### Raspberry Pi Pico (Pico / Pico W / Pico 2 / Pico 2 W)

| Function | Pin Name | Default GPIO | Physical Pin # | Details |
| :--- | :--- | :--- | :--- | :--- |
| **Button A** *(index 0)* | GP0 | `GPIO0` | Pin 1 | Active-low, internal pull-up |
| **D-pad DOWN** *(index 1)* | GP1 | `GPIO1` | Pin 2 | Active-low, internal pull-up |
| **D-pad LEFT** *(index 2)* | GP2 | `GPIO2` | Pin 4 | Active-low, internal pull-up |
| **D-pad RIGHT** *(index 3)* | GP3 | `GPIO3` | Pin 5 | Active-low, internal pull-up |
| **D-pad UP** *(index 4)* | GP4 | `GPIO4` | Pin 6 | Active-low, internal pull-up |
| **Buttons L + R** *(index 5)* | GP5 | `GPIO5` | Pin 7 | Active-low, triggers L and R simultaneously |
| **Activity LED** | GP25 / CYW43 | Board LED | Onboard | GP25 on Pico / Pico 2; CYW43 WL GPIO on Pico W / Pico 2 W |
| **Ground** | GND | `GND` | Pin 3, 8, 13, 18, 23, 28, 38 | Common side of every switch |

### ESP32-S3

| Function | ESP32-S3 GPIO | DevKitC-1 Physical Pin | Seeed Studio XIAO Pin | Details |
| :--- | :---: | :---: | :---: | :--- |
| **Button A** *(index 0)* | `GPIO1` | Row A, Pin 4 (silk `1`) | `D0` (Pin 1) | Active-low, internal pull-up |
| **D-pad DOWN** *(index 1)* | `GPIO2` | Row A, Pin 5 (silk `2`) | `D1` (Pin 2) | Active-low, internal pull-up |
| **D-pad LEFT** *(index 2)* | `GPIO3` | Row B, Pin 13 (silk `3`) | `D2` (Pin 3) | Active-low, internal pull-up |
| **D-pad RIGHT** *(index 3)* | `GPIO4` | Row B, Pin 4 (silk `4`) | `D3` (Pin 4) | Active-low, internal pull-up |
| **D-pad UP** *(index 4)* | `GPIO5` | Row B, Pin 5 (silk `5`) | `D4` (Pin 5) | Active-low, internal pull-up |
| **Buttons L + R** *(index 5)* | `GPIO6` | Row B, Pin 6 (silk `6`) | `D5` (Pin 6) | Active-low, triggers L and R simultaneously |
| **Activity LED** | `GPIO21` | Row A, Pin 18 (silk `21`) | Onboard Yellow LED | Active-low user indicator |
| **Ground** | `GND` | Row A, Pin 1/21/22 or Row B, Pin 22 (silk `G`) | `GND` (Pin 13) | Common side of every switch |

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
