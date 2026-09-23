# Teensy 4.x LED Blinker (`led-blinker`)

A minimal sample firmware project for **Teensy 4.1** and **Teensy 4.0** built with **PlatformIO**. It toggles the onboard orange LED every 500 ms and emits tick timestamps over USB CDC Serial.

---

## Hardware Pinout Reference

| Function | Logical Pin Name | Teensy 4.1 Physical Pin # | Teensy 4.0 Physical Pin # | Details |
| :--- | :--- | :---: | :---: | :--- |
| **Status LED** | `Pin 13` (`LED_BUILTIN`) | Pin 38 (or Onboard) | Pin 15 (or Onboard) | Active-high onboard orange LED (shared with SPI SCK) |
| **USB CDC Serial** | `Serial` | USB Micro-B Port | USB Micro-B Port | 480 Mbps High Speed USB CDC ACM serial console |

---

## Project Structure

```
teensy/led-blinker/
├── 00-build.sh         # Builds firmware for Teensy 4.1 (default) or Teensy 4.0
├── 01-install.sh       # Flashes the compiled firmware via teensy-cli
├── 02-monitor.sh       # Symlink to tools/monitor.sh (connects to USB serial console)
├── platformio.ini      # PlatformIO multi-environment configuration
├── src/
│   └── main.cpp        # Application entry point (LED blink + Serial output)
└── README.md
```

---

## Workflow Scripts

All workflow scripts adhere to the repository standard:

### 1. Build Firmware (`00-build.sh`)
Builds the firmware using PlatformIO. Defaults to `teensy41`, but accepts `-b teensy40`:

```bash
# Build for Teensy 4.1 (default)
./00-build.sh

# Build for Teensy 4.0
./00-build.sh -b teensy40

# Clean re-build
./00-build.sh -c
```

### 2. Flash Board (`01-install.sh`)
Flashes the compiled `.hex` binary to the Teensy using `teensy-cli` (installed and managed by PlatformIO):

```bash
# Flash Teensy 4.1 (will automatically invoke 00-build.sh if not built)
./01-install.sh

# Flash Teensy 4.0
./01-install.sh -b teensy40
```

> **Note:** If the board does not auto-reboot upon upload initiation, briefly press the physical pushbutton on the Teensy.

### 3. Monitor Serial Console (`02-monitor.sh`)
Connects to the Teensy's USB CDC ACM serial port (`/dev/ttyACM*`) at 115200 baud using available terminal tools (`tio`, `picocom`, `minicom`, or `python3 -m serial.tools.miniterm`):

```bash
./02-monitor.sh
```
