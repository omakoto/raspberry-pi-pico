# Raspberry Pi Pico 2 W BLE HID Multiplexer

A high-performance Bluetooth Low Energy (BLE) Human Interface Device (HID) Multiplexer and Dynamic Keymap Engine for the **Raspberry Pi Pico 2 W** (RP2350 + CYW43439).

This project connects wireless BLE keyboards and mice and aggregates them into a **single unified Composite USB HID device** (Keyboard + Mouse + VIAL WebHID) connected to your computer.

---

## Features

- **BLE Central / HOGP Host:**
  - Connects to Bluetooth Low Energy keyboards and mice (HID over GATT Profile).
  - Multi-device rollover aggregation: simultaneously merges modifier keys and keypresses across multiple keyboards without ghosting or stuck keys.
  - Rate-decoupled mouse vector aggregation (X, Y, scroll wheel, and 5 buttons) up to 1000 Hz.
  - Automatically reconnects to bonded devices across power cycles via non-volatile flash storage.
- **SSD1306 OLED Display (128×64 I2C):**
  - Displays connection status, connected peripheral names, active keymap layer, and USB connection health.
  - **Pairing Passkey Display:** When pairing a keyboard requiring Secure Simple Pairing / Passkey Entry, the 6-digit PIN is displayed prominently on the OLED (`Type 123456 + Enter on keyboard`).
- **Push Button Pairing Controller:**
  - Short press: Refresh status screen / clear toasts.
  - Long press (2s): Start BLE scanning and pairing mode (60-second discovery window).
  - Extra long press (8s): Factory reset (wipes all bonded BLE devices and resets keymap to default).
- **VIAL Dynamic Keymapping (WebHID):**
  - Exposes standard Vendor RawHID interface (`0xFF60:0x0061`).
  - Open [https://vial.rocks/](https://vial.rocks/) in Chrome to configure 4 layers, macros, tap-dance, and key remappings in real-time.
  - Keymaps are persisted across reboots in RP2350 flash memory.
- **Reverse Lock LED Sync:**
  - Forward CapsLock, NumLock, and ScrollLock status from the host PC back over BLE to connected keyboards.
- **Visual Heartbeat Indicator:**
  - Onboard wireless LED blinks at 1 Hz (500 ms on, 500 ms off) to verify microcontroller execution health.

---

## Hardware Wiring

Matches the pinout specification from [`circuitpython/ssd1306/`](../../circuitpython/ssd1306/README.md):

| Component | Pin Function | Pico 2 W GPIO | Physical Pin # | Notes |
| :--- | :--- | :--- | :--- | :--- |
| **SSD1306 OLED** | SDA | `GP2` | Pin 4 | I2C1 Data (400 kHz) |
| **SSD1306 OLED** | SCL | `GP3` | Pin 5 | I2C1 Clock |
| **SSD1306 OLED** | VCC / VDD | `3V3(OUT)` | Pin 36 | 3.3V DC power |
| **SSD1306 OLED** | GND | `GND` | Pin 3 or 8 | Common Ground |
| **Push Button** | Button Pin | `GP6` | Pin 9 | Active LOW (internal pull-up) |
| **Push Button** | Ground Pin | `GND` | Pin 8 or 13 | Connects pin to GND when pressed |
| **UART0 TX** | Serial Out | `GP16` | Pin 21 | 115,200 baud, 8N1 / Console & log output |
| **UART0 RX** | Serial In | `GP17` | Pin 22 | 115,200 baud, 8N1 / Console command input |
| **Host PC USB** | USB D+ / D- | Micro-USB | — | Upstream USB composite device |

---

## Building and Flashing

### 1. Build Firmware
```bash
./00-build.sh
```
Target outputs are generated in `build/`:
- `build/bt-hid-multiplexer.uf2`
- `build/bt-hid-multiplexer.elf`

### 2. Automated 1-Click Flashing
`01-install.sh` automatically drops the running board into BOOTSEL mode via USB CDC 1200-baud pulse or UART console and flashes the new binary:
```bash
./01-install.sh
```
*(No physical buttons or mode switching required!)* If flashing over a dedicated USB-to-UART adapter on GP16/GP17, you can specify:
```bash
MULTIPLEXER_UART=/dev/ttyUSB0 ./01-install.sh
```

### 3. Diagnostic Serial Console (USB CDC / UART0)
Interactive serial console and logger running concurrently on USB CDC (`/dev/ttyACM*`) and hardware UART0 (GP16/GP17 at 115,200 baud):
```bash
./02-monitor.sh
```
Commands supported on either console:
- `bootloader` / `bootsel`: Drops board directly into USB BOOTSEL ROM for updates
- `pair` / `scan`: Initiates 60-second BLE discovery pairing window
- `stop`: Halts BLE scanning
- `status`: Displays uptime, connection state, device name, and active layer
- `reset`: Clears all bonded devices and resets keymap to default
- `help`: Lists all console commands

---

## How to Pair a Device

1. **Enter Pairing Mode:**
   - Press and hold the push button on `GP6` for **2 seconds** until the OLED screen displays `BLE SCANNING...`.
2. **Put Peripheral in Pairing Mode:**
   - Put your Bluetooth keyboard or mouse into pairing / discoverable mode.
3. **Passkey Verification (Keyboards):**
   - For keyboards requiring numeric passkey authentication, the OLED displays:
     ```
     PAIR KEYBOARD PIN:
          849 201
     Type PIN + Enter on KB
     ```
   - Type the 6-digit number on the Bluetooth keyboard and press **Enter**.
4. **Connected:**
   - The OLED will display `BT: [Device Name]` and `LAYER 0: BASE`.
   - Your keyboard and mouse input will now be relayed to the PC over USB.

---

## Keymapping with VIAL

1. Connect the Pico 2 W upstream USB cable to your computer.
2. Open **[https://vial.rocks/](https://vial.rocks/)** in Google Chrome or any Chromium-based browser.
3. Click **Start** and select **Pico 2 W BLE HID Multiplexer**.
4. Remap keys across 4 layers (Base, Nav/Media, Function, NumPad). Changes take effect instantly and are stored in flash.
