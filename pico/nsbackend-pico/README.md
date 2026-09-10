# nsbackend-pico

A high-performance C++ port of **nsbackend** for the **Raspberry Pi Pico family** (**Pico**, **Pico 2**, **Pico W**, **Pico 2 W**), built on the **Raspberry Pi Pico SDK** and **FreeRTOS SMP**.

---

## 1. Overview

`nsbackend-pico` enables a Raspberry Pi Pico or Pico 2 microcontroller to operate as a high-performance Nintendo Switch controller backend:
1. **USB HID Gamepad**: Emulates a HORI Pokken Nintendo Switch controller over native USB (`VID: 0x0f0d`, `PID: 0x0092`), transmitting 8-byte HID reports to the Nintendo Switch.
2. **USB CDC ACM Serial Console**: Exposes a virtual serial console (`/dev/ttyACM0`) for real-time logging, status monitoring, and controller command input.
3. **USB MSC Flash Storage**: Exposes the internal 1MB FAT12 partition as a standard USB flash drive, allowing configuration editing of `config.toml` directly from your PC without re-flashing.
4. **Physical GPIO Buttons**: Active-low physical pushbuttons with internal pull-ups, 15ms debouncing, and opposing D-pad direction cancellation.
5. **USB-A Controller Pass-Through**: A secondary full-speed USB **host** port, bit-banged with PIO on GP16/GP17 and wired to a USB-A receptacle, lets you plug in a regular controller (Xbox 360/One/Series XInput pads, Nintendo Switch Pro Controller and Switch-mode third-party pads, DualShock 4, DualSense, generic DirectInput HID gamepads) and use it to control the Switch directly.
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
| (USB-A, XInput/HID)|          |    * Switch Pro Controller|
|                   |           |    * Generic HID gamepads |
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
- **Nintendo Switch Pro Controller** (official) and third-party controllers in Switch mode (e.g. 8BitDo, which present the same `057e:2009` identity). These use Nintendo's proprietary protocol: the firmware performs the USB handshake (including the baud renegotiation an official unit needs after a host reboot), switches the pad to full report mode 0x30, enables its IMU, reads its factory stick calibration, and decodes the packed 12-bit sticks; buttons map 1:1. The simple 0x3F report mode is also understood, and unacknowledged init steps are skipped so pads that only implement part of the protocol still work. Motion data is forwarded to the Switch when the board runs as a Pro Controller (see below).
- **Generic HID gamepads**: DualShock 4, DualSense, 8BitDo pads in D-input mode, wired "Switch compatible" third-party pads that use a plain HID report (e.g. DragonRise `0079:181d`), and other DirectInput-style USB gamepads. The HID report descriptor is parsed at connect time, so most pads work without per-device quirks.

Two enumeration robustness measures are built in, because cheap pads are picky: the first device-descriptor read is widened from TinyUSB's 8 bytes to the full descriptor (some pads drop off the bus after a short read), and a device that stays on the wire without ever completing enumeration is re-reset and re-enumerated every 1.5 s of bus idle instead of being abandoned.

### Motion controls: Pro Controller identity

The Switch only accepts motion data from a controller that speaks Nintendo's own protocol, so gyro pass-through requires the board to present itself as a **Nintendo Pro Controller** instead of the HORI Pokken pad. Enable it in `config.toml` (or the override file):

```toml
switch_identity = "procon"     # default: "pokken"
#procon_composite = true       # keep the CDC console and config drive attached (see below)
```

In this mode the native USB port enumerates as VID `057e` / PID `2009` with descriptors captured from a real Pro Controller, answers the console's USB handshake, subcommands and SPI-flash calibration reads, and streams the full 0x30 input report at about 60 Hz. Buttons, D-pad and sticks come from the same merged controller state as before (TCP/serial commands, GPIO, keypad, USB-A pass-through), and the accelerometer/gyroscope samples are forwarded from a Pro Controller plugged into the USB-A port. Without one attached, a resting orientation is reported so games see no motion rather than garbage.

Things to know about this mode:

- A real Pro Controller is a single-function USB device, so by default the CDC console and the MSC config drive are **not** exposed. Logging still works on UART0. `procon_composite = true` adds them back for experiments, at the risk of the console rejecting the device.
- Reflashing over USB therefore cannot use the CDC 1200-baud trick. `01-install.sh` falls back to sending `bootloader` over the UART0 command console when `NSBACKEND_UART` names the adapter's device (e.g. `NSBACKEND_UART=/dev/ttyUSB0 ./01-install.sh`); otherwise hold BOOTSEL while connecting.
- On Linux the `hid-nintendo` kernel driver drives the emulated controller exactly as the console does and exposes a joystick device plus a separate "(IMU)" event device, which makes a convenient test bench: `evtest` on the IMU device shows the forwarded motion.
- The console's IMU configuration is passed through to the attached controller: current system software enables the IMU in **mode 2** (subcommand 0x40 with argument 2), a newer sample layout than the classic mode 1 the Linux driver uses, and the sensitivity settings (subcommand 0x41) are forwarded as well. The attached controller is switched to whatever the console asked for and its IMU blocks are forwarded verbatim, one report per block with the controller's own timing, so motion matches the real controller in direction and speed. This has been verified on a Switch.
- The attached Pro Controller's factory stick calibration is read from its SPI flash during init so its sticks are centred correctly.

#### Background: the IMU and the Pro Controller protocol

**IMU** stands for inertial measurement unit: the motion sensor chip inside the controller. It combines an **accelerometer**, which measures linear acceleration along three axes including gravity (so it knows which way is down and how the pad is tilted or shaken), and a **gyroscope**, which measures angular velocity around three axes (how fast the pad is being rotated). Games fuse the two: the gyroscope provides fast, precise rotation for aiming, and the accelerometer's gravity vector corrects the slow drift the gyroscope accumulates. "Gyro aiming" and "motion controls" both read this chip.

The Pro Controller speaks a proprietary protocol on top of USB HID, documented mostly by the community (the dekuNukem *Nintendo_Switch_Reverse_Engineering* notes and the Linux `hid-nintendo` driver source). The parts this firmware relies on:

- **USB commands** (output report `0x80`): handshake (`02`), set the internal link to 3 Mbps (`03`), stop the Bluetooth fallback timer (`04`), status/MAC query (`01`). The console also sends `05` and empty probe packets before the handshake.
- **Subcommands** (output report `0x01`: sequence counter, 8 rumble bytes, subcommand id, arguments), each answered by input report `0x21` carrying an acknowledgement byte, the subcommand id and reply data. Used here: device info (`02`), set input report mode (`03`), trigger timings (`04`), shipment flag (`08`), SPI flash read (`10`, how the console fetches stick and IMU calibration and colours), NFC/IR MCU configuration (`21`), player lights (`30`), enable IMU (`40`), IMU sensitivity (`41`), enable vibration (`48`), voltage (`50`), Bluetooth pairing steps (`01`).
- **Input report `0x30`**: timer, battery/connection byte, three button bytes, two packed 12-bit sticks, vibration status, then **36 IMU bytes**, padded to 64. The controller emits one about every 15 ms.

The `enable IMU` subcommand's argument selects how those 36 bytes are laid out:

- **Mode 1** (argument `1`): three samples taken 5 ms apart, each six little-endian 16-bit integers: accelerometer X, Y, Z then gyroscope X, Y, Z, as raw sensor counts. The receiver converts them with the calibration it read from SPI flash (per-axis origin and sensitivity). This is the classic, fully documented format, the one `hid-nintendo` requests, and the one this firmware's host side parses.
- **Mode 2** (argument `2`): a newer layout introduced by later controller firmware and requested by current Switch system software. Its byte encoding is **not** described in the documentation this firmware was written from. What is known empirically: an official Pro Controller acknowledges the request, the console decodes the resulting blocks correctly, and feeding it mode-1 blocks instead produces erratic motion that never settles.

Because mode 2 is opaque, the emulation never interprets or generates IMU data for the console. It forwards the console's mode and sensitivity requests to the attached controller and copies that controller's IMU blocks byte for byte, one report per block, together with the controller's own timer byte, so the console receives exactly the stream a directly connected controller would produce. Anything that would require the board to *create* motion data, such as text commands for gyro input, first needs the mode-2 encoding to be reverse-engineered (for example by capturing the same physical motion from a real controller in both modes and comparing).

#### Known limitations

- **IMU calibration**: the console is served the IMU calibration of the reference unit the emulation was captured from, not the attached controller's own. With a different controller attached, the console sees that controller's raw drift interpreted through the reference calibration. Recalibrating motion controls in the console's system settings compensates for it per session, but the console's calibration writes are acknowledged without being stored, so they do not survive a power cycle.
- **Rumble** commands from the console are acknowledged but not forwarded to the attached controller.
- **IMU mode 2 is required**: the console enables the newer sample layout, and the board passes that request to the attached controller instead of converting samples. An official Pro Controller supports it; a third-party pad that rejects mode 2 will deliver motion the console cannot decode (erratic movement even at rest). Converting mode-1 samples on the board is a possible future addition.
- **Single-function USB by default**: in this mode there is no CDC console and no config drive. Logging is on UART0, and reflashing uses `NSBACKEND_UART=/dev/ttyUSB0 ./01-install.sh` (or the BOOTSEL button). Editing the configuration means switching back to `switch_identity = "pokken"`, using the drive, and switching again, unless `procon_composite = true` is acceptable for your console.
- **Attached controller only**: motion comes solely from a Pro Controller on the USB-A port. Xbox pads and DirectInput pads provide buttons and sticks but no motion, and with nothing attached the emulation reports a resting orientation.
- **Power**: the USB-A port is fed from the Pico's VBUS. A battery-equipped Pro Controller also charges from it, so on a console port with a tight current budget, powering the USB-A connector's VBUS from a separate 5 V supply (common ground) avoids the attached controller dropping off the bus.

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

### Crash reports on UART0

A HardFault on either core, or a pico-sdk `panic()` (for example a PIO, DMA or alarm claim that collides with another driver), prints a diagnostic on **UART0 (GP12 TX, 115200 baud)** and reboots the board three seconds later, instead of stopping silently the way the stock SDK handlers do:

```
[E][Fault] HardFault on core 0
  PC=0x1000a2c6 LR=0x1000a1f1 xPSR=0x61000000 EXC_RETURN=0xfffffffd
  R0=... R1=... R2=... R3=... R12=...
  CFSR=0x00008200 HFSR=0x40000000 MMFAR=... BFAR=0xfffffff0
[E][Fault] panic on core 1: DMA channel 0 is already claimed
[E][Fault] rebooting in 3 s
```

`PC` is the faulting instruction; look it up with `arm-none-eabi-addr2line -e build/nsbackend-pico.elf <PC>`. The `CFSR`/`BFAR` line only exists on RP2350 boards. FreeRTOS stack overflows and heap exhaustion are reported the same way (see `main.cpp`).

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
