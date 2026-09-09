# nsbackend-pico

A high-performance C++ port of **nsbackend** for the **Raspberry Pi Pico family** (**Pico**, **Pico 2**, **Pico W**, **Pico 2 W**), built on the **Raspberry Pi Pico SDK** and **FreeRTOS SMP**.

---

## 1. Overview

`nsbackend-pico` enables a Raspberry Pi Pico or Pico 2 microcontroller to operate as a high-performance Nintendo Switch controller backend:
1. **USB HID Gamepad**: Emulates a HORI Pokken Nintendo Switch controller over native USB (`VID: 0x0f0d`, `PID: 0x0092`), transmitting 8-byte HID reports to the Nintendo Switch.
2. **USB CDC ACM Serial Console**: Exposes a virtual serial console (`/dev/ttyACM0`) for real-time logging, status monitoring, and controller command input.
3. **USB MSC Flash Storage**: Exposes the internal 1MB FAT12 partition as a standard USB flash drive, allowing configuration editing of `config.toml` directly from your PC without re-flashing.
4. **Physical GPIO Buttons**: Active-low physical pushbuttons with internal pull-ups, 15ms debouncing, and opposing D-pad direction cancellation.
5. **USB-A Controller Pass-Through**: A secondary full-speed USB **host** port, bit-banged with PIO on GP16/GP17 and wired to a USB-A receptacle, lets you plug in a regular PC controller (Xbox 360/One/Series XInput pads, DualShock 4, DualSense, generic DirectInput HID gamepads) and use it to control the Switch directly.
6. **Dual Serial Command Server**: Concurrently accepts controller commands on hardware UART0 (GP12 TX / GP13 RX at 115,200 baud) and USB CDC ACM serial, while echoing logs to both channels.
7. **Status LED State Machine**: Visual status indicators via onboard LED (GP25 on Pico/Pico 2, CYW43 wireless GPIO on Pico W/Pico 2 W).
8. **Wireless Networking (Pico W / Pico 2 W only)**:
   - **Multi-AP Wi-Fi Manager**: Automatic connection to configured access points with background scanning and seamless auto-reconnect.
   - **lwIP mDNS Service**: Advertises `nscon.local` with service `_nscon._tcp` on port `10100`.
   - **TCP Command Server**: High-throughput BSD socket server on port `10100` for streaming controller commands from frontend clients (`nsfrontend` or scripts).

---

## 2. Hardware Pinout

| Function | Pin Name | Default GPIO | Physical Pin # | Details |
| :--- | :--- | :--- | :--- | :--- |
| **Button A** | GP0 | `GPIO0` | Pin 1 | Active-low, internal pull-up |
| **D-pad DOWN** | GP1 | `GPIO1` | Pin 2 | Active-low, internal pull-up |
| **D-pad LEFT** | GP2 | `GPIO2` | Pin 4 | Active-low, internal pull-up |
| **D-pad RIGHT** | GP3 | `GPIO3` | Pin 5 | Active-low, internal pull-up |
| **D-pad UP** | GP4 | `GPIO4` | Pin 6 | Active-low, internal pull-up |
| **Button B** | GP5 | `GPIO5` | Pin 7 | Active-low, internal pull-up |
| **Buttons L + R** | GP10 | `GPIO10` | Pin 14 | Active-low, triggers L and R simultaneously |
| **USB Host D+** | GP16 | `GPIO16` | Pin 21 | USB-A receptacle D+ (green), PIO-USB host port (configurable via `usb_host_dp_pin`) |
| **USB Host D-** | GP17 | `GPIO17` | Pin 22 | USB-A receptacle D- (white), always `usb_host_dp_pin` + 1 |
| **USB Host VBUS** | VBUS | `VBUS` | Pin 40 | 5V from the Pico's USB supply to the USB-A receptacle VBUS (red) |
| **UART0 TX** | GP12 | `GPIO12` | Pin 16 | 115,200 baud, 8N1 / Serial log output |
| **UART0 RX** | GP13 | `GPIO13` | Pin 17 | 115,200 baud, 8N1 / Serial command input |
| **I2C0 SDA** | GP20 | `GPIO20` | Pin 26 | PCF8574 Keypad SDA (configurable via `i2c_sda_pin`) |
| **I2C0 SCL** | GP21 | `GPIO21` | Pin 27 | PCF8574 Keypad SCL (configurable via `i2c_scl_pin`) |
| **Status LED** | GP25 / CYW43 | Board LED | Onboard | GP25 on Pico / Pico 2; CYW43 WL GPIO on Pico W / Pico 2 W |
| **USB** | D+ / D- | Native USB | USB Port | Standard micro-USB (Pico) or USB-C connector |
| **3.3V Power** | 3V3(OUT) | `3V3` | Pin 36 | 3.3V DC power for external peripherals (e.g. I2C keypad) |
| **GND** | GND | `GND` | Pin 3, 8, 13, 18, 23, 28, 38 | Digital ground |

---

## 3. Status LED Patterns

| State | Lifecycle Phase | LED Blink Pattern |
| :--- | :--- | :--- |
| **Initializing** | Power-on, flash mount, TinyUSB init | **Solid ON** |
| **Wi-Fi Connecting** *(W boards)* | Scanning and associating with Wi-Fi AP | **0.1s ON, 1.0s OFF** (1 short blink) |
| **Setting up TCP** *(W boards)* | Wi-Fi connected, starting mDNS & TCP | **0.1s ON, 0.1s OFF, 0.1s ON, 1.0s OFF** (2 short blinks) |
| **Waiting for Client** | Ready / TCP listening on port 10100 | **0.5s ON, 0.5s OFF** (Slow pulse) |
| **Client Connected** *(W boards)* | TCP client connected & streaming | **1.0s ON, 1.0s OFF** (Heartbeat) |
| **Wi-Fi Error / Reconnecting** | Wi-Fi failed / auto-reconnecting | **0.1s ON, 0.1s OFF** (Rapid strobe) |

---

## 4. Architecture

```
                                 +---------------------------+
                                 |      Nintendo Switch      |
                                 +-------------^-------------+
                                               | (USB HID Gamepad)
+-------------------+           +-------------v-------------+
|    TCP Client     |  (Wi-Fi)  |      nsbackend-pico       |
| (nsfrontend / CLI)| --------> |  - TCP Server (Port 10100)|
+-------------------+           |  - Controller State Engine|
                                |  - Composite TinyUSB:     |
+-------------------+           |    * HORI Gamepad HID     |
|   UART0 / USB CDC | --------> |    * CDC Serial Console   |
|   Serial Console  |           |    * MSC Storage (FATFS)  |
+-------------------+           |  - Dual Logger (UART/CDC) |
|  Physical Buttons | --------> |  - GPIO Button Debouncer  |
|   (GP0-GP5, GP10) |           |  - I2C Keypad (PCF8574)   |
+-------------------+           |  - CYW43 Wi-Fi & lwIP mDNS|
| I2C Matrix Keypad | --------> |  - FreeRTOS SMP (Dual Core|
|  (4x4 on GP20/21) |           |  - 1MB Flash FAT12 DiskIO |
+-------------------+           |  - PIO-USB Host (GP16/17) |
|   PC Controller   | --------> |    * XInput (Xbox pads)   |
| (USB-A, XInput/HID)|          |    * Generic HID gamepads |
+-------------------+           +---------------------------+
```

---

## 5. I2C Matrix Keypad (PCF8574 / PCF8574A)

Supports standard 4x4 matrix keypads interfaced through an I2C PCF8574 / PCF8574A expander module.

### Hardware Connections
- **SDA**: `GP20` (Pin 26, default, configurable via `i2c_sda_pin`)
- **SCL**: `GP21` (Pin 27, default, configurable via `i2c_scl_pin`)
- **VCC**: 3.3V
- **GND**: GND
- **I2C Address**: `0x20` (standard PCF8574) or `0x38` (PCF8574A, auto-detected fallback)

### Key Assignment
| Keypad Key | Target Controller Input | Logged Command | Description |
| :---: | :---: | :---: | :--- |
| **`2`** | **D-pad UP** | `pu [Up]` | Directional Pad UP |
| **`4`** | **D-pad LEFT** | `pl [Left]` | Directional Pad LEFT |
| **`6`** | **D-pad RIGHT** | `pr [Right]` | Directional Pad RIGHT |
| **`8`** | **D-pad DOWN** | `pd [Down]` | Directional Pad DOWN |
| **`1`** | **L1** (`BTN_L`) | `l1 [L]` | Left bumper |
| **`3`** | **R1** (`BTN_R`) | `r1 [R]` | Right bumper |
| **`7`** | **L2** (`BTN_ZL`) | `l2 [ZL]` | Left trigger (ZL) |
| **`9`** | **R2** (`BTN_ZR`) | `r2 [ZR]` | Right trigger (ZR) |
| **`*`** | **Minus** (`BTN_MINUS`) | `m [Minus]` | Select / Minus |
| **`#`** | **Plus** (`BTN_PLUS`) | `p [Plus]` | Start / Plus |
| **`A`** | **A** (`BTN_A`) | `a [A]` | Button A |
| **`B`** | **B** (`BTN_B`) | `b [B]` | Button B |
| **`C`** | **X** (`BTN_X`) | `x [X]` | Button X |
| **`D`** | **Y** (`BTN_Y`) | `y [Y]` | Button Y |
| **`0`** | **Home** (`BTN_HOME`) | `h [Home]` | Home Button |
| **`5`** | *(Unused)* | *(None)* | Ignored |

---

## 6. USB-A Controller Pass-Through

Plug a regular PC controller into the USB-A host port and it acts directly as a Switch controller, merged with all other input sources (TCP/serial commands, GPIO buttons, I2C keypad).

Because the Pico's native USB port is occupied by the Switch-facing composite device, the host port is a second full-speed USB port bit-banged with PIO (Pico-PIO-USB) on two GPIOs. This also requires the system clock to run at 120 MHz (a multiple of the 12 MHz USB bit clock), which the firmware sets at boot.

### Wiring the USB-A receptacle

| USB-A Pin | Wire Color | Connect To | Physical Pin # |
| :--- | :--- | :--- | :--- |
| VBUS (1) | Red | VBUS (5V) | Pin 40 |
| D- (2) | White | GP17 | Pin 22 |
| D+ (3) | Green | GP16 | Pin 21 |
| GND (4) | Black | GND | Pin 38 (or any GND) |

Keep the D+/D- wires short (a few cm) and equal length. The D+/D- GPIOs are configurable via `usb_host_dp_pin` (D- is always D+ + 1). A small USB hub between the port and the controller also works.

### Supported controllers

- **XInput**: Xbox 360 / Xbox One / Xbox Series wired controllers (and the Xbox 360 wireless receiver), via the vendored [tusb_xinput](https://github.com/Ryzee119/tusb_xinput) host driver.
- **Generic HID gamepads**: DualShock 4, DualSense, 8BitDo pads in D-input mode, and other DirectInput-style USB gamepads. The HID report descriptor is parsed at connect time, so most pads work without per-device quirks.

### Button mapping

Face buttons are mapped **positionally** (by physical location, not by label), so muscle memory carries over:

| Physical Position | Xbox | PlayStation | Acts as Switch |
| :--- | :--- | :--- | :--- |
| Bottom | A | Cross | B |
| Right | B | Circle | A |
| Left | X | Square | Y |
| Top | Y | Triangle | X |

Shoulders map L1/LB → L, R1/RB → R, L2/LT → ZL, R2/RT → ZR; Back/Share/Select → Minus, Start/Options → Plus, Guide/PS → Home, Xbox Share button / PS touchpad click → Capture. Sticks (including click) and the D-pad pass through directly, with a configurable radial deadzone (`usb_host_deadzone_percent`).

---

## 7. Supported Boards

| Board Target | MCU Architecture | Wireless Support | Default Build Command |
| :--- | :--- | :--- | :--- |
| **`pico2_w`** *(default)* | RP2350 (Dual ARM Cortex-M33) | CYW43439 (Wi-Fi 4 + BLE) | `./00-build.sh` |
| **`pico2`** | RP2350 (Dual ARM Cortex-M33) | None (Serial/USB control) | `./00-build.sh -b pico2` |
| **`pico_w`** | RP2040 (Dual ARM Cortex-M0+) | CYW43439 (Wi-Fi 4 + BLE) | `./00-build.sh -b pico_w` |
| **`pico`** | RP2040 (Dual ARM Cortex-M0+) | None (Serial/USB control) | `./00-build.sh -b pico` |

### Memory Footprint & Architecture Differences

- **RP2350 (`pico2_w`, `pico2`)**: 512KB SRAM allows a 320KB FreeRTOS heap and expanded lwIP buffer pools (32 pbufs, 16KB TCP heap, 8×MSS windows).
- **RP2040 (`pico_w`, `pico`)**: 264KB SRAM (256KB main RAM). On `pico_w`, lwIP static buffer pools are sized to 12 pbufs with an 8KB TCP heap and 4×MSS windows in `lwipopts.h`, allowing the full network stack to fit alongside a 160KB FreeRTOS heap with over 40KB of headroom for the C heap and runtime stacks.

---

## 8. Building & Installation

### Prerequisites
- Raspberry Pi Pico SDK (`v2.1.1` or later)
- ARM GNU Embedded Toolchain (`arm-none-eabi-gcc` / `g++`)
- FreeRTOS Kernel (`FreeRTOS-Kernel`)
- CMake (`>= 3.13`) and Ninja / Make

### 1. Build Firmware & Storage Image
```bash
cd ~/cbin/src/raspberry-pi-pico/pico/nsbackend-pico

# Build for Pico 2 W (default):
./00-build.sh

# Or build for Pico W:
./00-build.sh -b pico_w -c

# Or build clean for non-wireless Pico 2:
./00-build.sh -b pico2 -c

# Or build clean for non-wireless Pico:
./00-build.sh -b pico -c
```

The build produces a single combined UF2 image in `build/`:
- `nsbackend-pico.uf2`: Combined UF2 containing both the firmware binary (at `0x10000000`) and the 1MB FAT12 storage partition (at `0x10100000`) pre-populated with `config.toml` and `config-override.toml`.
- `storage.bin` / `storage.uf2`: Standalone FAT filesystem image and UF2 partition.

### 2. Flashing
Hold down the **BOOTSEL** button on your Pico while plugging it into your computer's USB port (the board mounts as a drive named `RPI-RP2` or `RP2350`).

Run:
```bash
./01-install.sh
```
Or simply copy `build/nsbackend-pico.uf2` directly into the mounted drive.

> [!NOTE]
> Even if flash memory is completely erased or unformatted, the firmware automatically formats the 1MB partition as FAT12 and restores default `config.toml` and `config-override.toml` on first boot.

---

## 9. Configuration (`config.toml`)

When plugged into a PC via USB, the Pico exposes a standard USB flash drive with `config.toml`. You can open and edit this file directly in any text editor.

Sample `config.toml`:
```toml
hostname = "nscon"
tcp_port = 10100
log = true
enable_echo = true
led_active_low = false

# Primary Wi-Fi Access Point (Index 0)
# 'wifi_ssid0' and 'wifi_password0' are also accepted as aliases for index 0.
wifi_ssid = "MyHomeNetwork"
wifi_password = "SecretPassword123"

# Fallback Wi-Fi Access Points (Indices 1 to 9)
wifi_ssid1 = "MobileHotspot"
wifi_password1 = "BackupPassword456"

# I2C Matrix Keypad (PCF8574 / PCF8574A)
i2c_keypad_enabled = true
i2c_sda_pin = 20
i2c_scl_pin = 21
i2c_address = 0x20
i2c_reverse_row = true
i2c_reverse_col = true
i2c_debounce_ms = 20

# USB Host Port (USB-A controller pass-through)
usb_host_enabled = true
usb_host_dp_pin = 16
usb_host_deadzone_percent = 10
```

---

## 10. Serial Console Monitoring

Run the monitor script to view debug logs over USB CDC:
```bash
./02-monitor.sh
# or specify a specific device:
./02-monitor.sh /dev/ttyACM0
```

---

## 11. Controller Command Protocol

Commands can be transmitted over TCP (port 10100) or over serial (UART0 / USB CDC).

### Button Commands
```text
a              # Press and auto-release button A
b              # Press and auto-release button B
x              # Press and auto-release button X
y              # Press and auto-release button Y
l1             # Left bumper (L)
r1             # Right bumper (R)
l2             # Left trigger (ZL)
r2             # Right trigger (ZR)
p, plus        # Plus / Start button
m, minus       # Minus / Select button
h, home        # Home button
c, capture     # Capture / Screenshot button
pu             # D-pad UP
pd             # D-pad DOWN
pl             # D-pad LEFT
pr             # D-pad RIGHT
```

### Analog Stick Commands
Coordinates range from `-1.0` to `1.0`:
```text
lx 0.75        # Left stick X axis
ly -0.50       # Left stick Y axis
rx -1.00       # Right stick X axis
ry 1.00        # Right stick Y axis
```

### Duration Commands
Append duration in seconds to hold the input before auto-releasing:
```text
a 0.20         # Hold button A for 200 milliseconds
pu 0.15        # Hold D-pad UP for 150 milliseconds
```

---

## 12. Latency Benchmarking

To benchmark network round-trip latency over Wi-Fi:
```bash
./measure-latency.py --host nscon.local --count 100 --stream-samples 200
```
*(Ensure `enable_echo = true` is set in `config.toml`)*
