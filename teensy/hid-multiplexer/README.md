# Teensy 4.1 USB HID Multiplexer (`hid-multiplexer`)

A high-performance USB HID keyboard and mouse multiplexer firmware for **Teensy 4.1** built on **PlatformIO** and **`USBHost_t36`**. It aggregates discrete input streams from multiple physical USB keyboards and mice connected via an external USB hub into a single unified composite USB HID interface to the host computer, with dynamic layer remapping and macro support.

---

## 1. Overview & Architecture

```
+-----------------------------------+       +-----------------------------------+
|       USB Keyboard 1 / 2          |       |          USB Mouse 1 / 2          |
+-----------------+-----------------+       +-----------------+-----------------+
                  |                                           |
                  +---------------------+---------------------+
                                        |
                                        v
                          [ Downstream USB 2.0 Hub ]
                                        |
                                        v (5-pin USB Host Header)
+-------------------------------------------------------------------------------+
|                       Teensy 4.1 Multiplexer Engine                           |
|                                                                               |
|  [ USBHost_t36 EHCI Host Controller & Hub Driver ]                            |
|       - Hotplug & Connection Watchdog (Purges stuck keys on detach)           |
|                                                                               |
|  [ Input Aggregation Engine ]                                                 |
|       - Keyboard: Bitwise OR modifiers, active keycode set union (6KRO)       |
|       - Mouse: Bitwise OR buttons, relative delta (dX/dY/dZ) summation        |
|       - Jitter Prevention: Rate-decoupled 1000 Hz timer flush                 |
|       - Reverse Lock LED Synchronization (CapsLock/NumLock feedback)          |
|                                                                               |
|  [ Virtual Matrix & Dynamic Layer Engine ]                                    |
|       - 16x16 Virtual Matrix (256 keyslots)                                   |
|       - 4 Layers (MO, TG, TO, OSL layer switching actions)                    |
|       - Cross-Device Layer Activation (e.g. Fn on Keyboard 1 remaps Mouse 1)  |
|       - Non-volatile Flash/EEPROM persistence across power cycles             |
|                                                                               |
|  [ VIAL / VIA WebHID & Serial CLI Server ]                                    |
|       - WebHID protocol server for https://vial.rocks                         |
|       - Real-time Serial CLI command interpreter                              |
+---------------------------------------+---------------------------------------+
                                        |
                                        v (Native Micro-USB Cable)
                        +-------------------------------+
                        |     Target Host Computer      |
                        |                               |
                        | Interface 0/1: CDC Serial     |
                        | Interface 2:   HID Keyboard   |
                        | Interface 3:   HID Mouse      |
                        | Interface 5:   Media Keys     |
                        +-------------------------------+
```

---

## 2. Hardware Wiring (Teensy 4.1 USB Host Header)

The Teensy 4.1 features an onboard 5-pin USB Host header (`J1`) located directly below the NXP i.MX RT1062 processor:

| Pin Name | Description | Connects To (USB Hub / Receptacle) | Details |
| :--- | :--- | :--- | :--- |
| **5V (VBUS)** | 5V Power Output | USB Hub VBUS (Red wire) | 5V power supply to downstream devices (≤500mA limit from USB) |
| **D-** | USB Data Minus | USB Hub D- (White wire) | High-speed differential pair |
| **D+** | USB Data Plus | USB Hub D+ (Green wire) | High-speed differential pair |
| **GND** | Digital Ground | USB Hub GND (Black wire) | System ground reference |
| **GND** | Shield Ground | Cable Shield / GND | Earth/chassis shield ground |

> [!TIP]
> **Powered USB Hub Recommended:** When attaching multiple RGB gaming keyboards or high-polling mice, use an externally-powered USB hub to supply 5V to the peripherals, avoiding excessive current draw through the Teensy's onboard 5V trace.

### Hardware UART & Remote Flashing Interface (Serial1)

To allow serial monitoring, runtime debugging, and remote bootloader reboots when running inside virtualization environments (e.g. VMware) where USB HID devices are captured by the host, the Teensy 4.1 is equipped with a hardware UART console on **UART1 (`Serial1`)**:

| Teensy 4.1 Pin | Signal | External USB-to-UART Adapter | Details |
| :--- | :--- | :--- | :--- |
| **Pin 0** | `RX1` (Input) | **TX** | Receives CLI commands (e.g. `bootloader`, `status`, `remap`) |
| **Pin 1** | `TX1` (Output) | **RX** | Transmits diagnostic logs and command responses (115200 baud) |
| **GND** | Ground | **GND** | Common ground reference |

Dual-channel logging broadcasts all output simultaneously to **Native USB CDC (`Serial`)** and **Hardware UART (`Serial1`)**.

---

## 3. Workflow Scripts

Following the repository standard, all operations are managed through root workflow scripts:

### A. Build Firmware (`00-build.sh`)
Compiles the firmware for Teensy 4.1 using PlatformIO:
```bash
./00-build.sh

# Clean rebuild
./00-build.sh -c
```
Target binaries are generated in `.pio/build/teensy41/`:
- `firmware.hex`
- `firmware.elf`

### B. Flash Firmware (`01-install.sh`)
Flashes the compiled `.hex` image to the Teensy board via `teensy-cli`:
```bash
./01-install.sh

# Or trigger reboot remotely via hardware UART adapter:
./01-install.sh --uart /dev/ttyUSB0
```
> [!NOTE]
> If running inside a virtual machine (e.g. VMware) where USB HID is captured by the host, `01-install.sh` can send the `bootloader` command over an external USB-to-UART adapter to enter HalfKay mode automatically, or you can press the physical button on the Teensy.

### C. Serial Monitor & CLI (`02-monitor.sh`)
Connects to the Teensy's serial console (either native USB CDC `/dev/ttyACM*` or hardware UART `/dev/ttyUSB*`) at 115200 baud:
```bash
./02-monitor.sh

# Or explicitly specify device:
./02-monitor.sh --uart /dev/ttyUSB0
```

---

## 4. Input Multiplexing Specifications

### Keyboard Aggregation
- **Modifier Union:** The modifier byte sent to the host PC is the bitwise OR of all connected keyboards:
  $$\text{Mod}_{\text{out}} = \text{Mod}_1 \mid \text{Mod}_2 \mid \dots \mid \text{Mod}_N$$
- **Keycode Set Union (6KRO):** Active keycodes from all keyboards are combined into a 6-slot rollover report.
- **Connection Watchdog:** If a keyboard is disconnected while keys are physically held, the multiplexer automatically purges all registered keys for that device and transmits a release report, preventing **stuck key repeats**.
- **Lock LED Sync:** When the target PC toggles Caps Lock, Num Lock, or Scroll Lock, the status is immediately propagated downstream to all connected keyboards.

### Mouse Aggregation
- **Button Union:** Mouse buttons are merged via bitwise OR:
  $$\text{Buttons}_{\text{out}} = \text{Buttons}_1 \mid \text{Buttons}_2 \mid \dots \mid \text{Buttons}_N$$
- **Delta Summation:** Relative cursor displacements ($\Delta X, \Delta Y, \Delta \text{Wheel}$) from all mice are summed and clamped to $[-127, +127]$.
- **Jitter-Free Rate Decoupling:** Mouse movements accumulate in atomic counters and are flushed to the PC on a steady 1000 Hz (1ms) timer tick.

---

## 5. Dynamic Layer Remapping & Serial CLI

The multiplexer maintains a 4-layer dynamic keymap in non-volatile flash storage (EEPROM emulation) that can be configured in real time without recompiling:

### Serial CLI Commands (`./02-monitor.sh`)
| Command | Arguments | Description |
| :--- | :--- | :--- |
| `status` | None | Displays uptime, active layer, and Caps/Num Lock status |
| `dump` | `<layer>` (0–3) | Dumps the $16 \times 16$ keycode matrix for the specified layer |
| `remap` | `<layer> <row> <col> <hex_keycode>` | Remaps a specific keyslot (e.g. `remap 0 0 4 0005` maps 'A' to 'B') |
| `reset` | None | Resets keymap to factory 1:1 default mapping |
| `help` | None | Displays available commands |

### Supported QMK Action Opcodes
- `MO(layer)` (`0x5220 | layer`): Momentary layer activation while held.
- `TG(layer)` (`0x5240 | layer`): Toggles layer on/off.
- `TO(layer)` (`0x5200 | layer`): Switches default layer.

---

## 6. VMware Virtualization Support & Troubleshooting

### Why VMware Hides the Teensy
When the Teensy boots into `hid-multiplexer` firmware, it enumerates as a USB composite device declaring USB CDC Serial alongside USB HID Keyboard and Mouse interfaces (`-DUSB_SERIAL_HID`). By default, VMware's USB Arbitrator automatically captures any USB device containing an HID Keyboard (Interface Class 0x03, Protocol 1) or Mouse (Interface Class 0x03, Protocol 2) to protect host input integrity, hiding the entire device (including its CDC serial port) from the guest VM.

### Solutions

#### Solution 1: Use Hardware UART for Flashing & Monitoring (No VM Configuration Required)
Connect an inexpensive USB-to-UART dongle (CP2102, FT232, CH340, etc.) to the Teensy's Pin 0 (RX1) and Pin 1 (TX1) with GND:
1. Since the USB-to-UART adapter only presents a standard USB-to-Serial interface without HID descriptors, VMware passes it into the guest VM without interference as `/dev/ttyUSB0`.
2. All logs from `logger_printf` are mirrored to `Serial1`.
3. To flash new firmware, `01-install.sh --uart /dev/ttyUSB0` sends `bootloader\n` to `Serial1`, triggering `_reboot_Teensyduino_()`. Once in the HalfKay bootloader (`16c0:0478`), VMware allows passing the device through to the VM because HalfKay is a vendor-specific HID device.

#### Solution 2: Enable Generic HID Passthrough in VMware (`.vmx`)
If you prefer passing the native Teensy USB composite device directly to the guest VM:
1. Shut down the virtual machine.
2. Open the VM's `.vmx` configuration file on the host machine.
3. Append the following directive:
   ```ini
   usb.generic.allowHID = "TRUE"
   ```
4. Start the VM. VMware will now permit passing through HID keyboards and composite devices under **VM > Removable Devices**.

