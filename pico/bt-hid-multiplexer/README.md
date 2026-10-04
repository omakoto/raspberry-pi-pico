# Raspberry Pi Pico 2 W / Pico W BLE HID Multiplexer

A high-performance Bluetooth Low Energy (BLE) Human Interface Device (HID) Multiplexer and Dynamic Keymap Engine for the **Raspberry Pi Pico 2 W** (RP2350 + CYW43439) and **Raspberry Pi Pico W** (RP2040 + CYW43439).

This project connects wireless BLE keyboards, mice, and integrated trackpads and aggregates them into a **single unified Composite USB HID device** (Keyboard + Mouse + VIAL WebHID) connected to your computer.

---

## Features

- **Bluetooth Classic (BR/EDR) HID Host:**
  - Connects keyboards, numeric keypads, trackpads and mice that only speak the classic HID profile and are therefore invisible to the BLE scan (up to 4 at once, `MAX_CLASSIC_DEVICES`). Their input is merged into the same USB output as the BLE devices.
  - Pairing mode also runs a classic inquiry. See [Bluetooth Classic devices](#bluetooth-classic-devices).
- **BLE Central / HOGP Host:**
  - Connects to Bluetooth Low Energy keyboards, mice, and composite keyboard+trackpad peripherals (HID over GATT Profile).
  - **Integrated Trackpad & Mouse Support:** Supports multi-touch trackpads (e.g. ProtoArc XK01 TP) with 16-bit relative X/Y motion vectors, vertical scroll wheel, horizontal pan, standard 8-bit mice, and Logitech 12-bit packed coordinate mice (e.g. Logitech Lift, MX Master).
  - **Configurable Mouse Sensitivity:** Scale mouse cursor speed globally or per-slot via `mousespeed <percent>` with sub-count fractional remainder accumulation to avoid dropping micro-movements.
  - Multi-device rollover aggregation: simultaneously merges modifier keys and keypresses across multiple keyboards without ghosting or stuck keys.
  - Rate-decoupled mouse and trackpad vector aggregation (X, Y, scroll wheel, pan, and 5 buttons) up to 1000 Hz.
  - Automatically reconnects to bonded devices across power cycles via non-volatile flash storage.
- **SSD1306 OLED Display (128×64 I2C):**
  - Displays connection status, connected peripheral names, active keymap layer, and USB connection health.
  - **Pairing Passkey Display:** When pairing a keyboard requiring Secure Simple Pairing / Passkey Entry, the 6-digit PIN is displayed prominently on the OLED (`Type 123456 + Enter on keyboard`).
- **Push Button Pairing Controller:**
  - Short press: Toggle BLE pairing mode on/off (60-second discovery window).
  - Long press (2s): Start BLE pairing mode (60-second discovery window).
  - Extra long press (8s): Factory reset (wipes all bonded BLE devices and resets keymap to default).
  - **Non-Aggressive Pairing:** Background scanning strictly reconnects to already-bonded peripherals and will never hijack unbonded devices advertising in pairing mode nearby. New devices are only discovered and paired during an active Pairing Mode window.
- **LRU Active Connection Eviction (Up to 8 Concurrent Devices):**
  - Supports up to 8 active BLE connections simultaneously (`MAX_BLE_DEVICES = 8`).
  - Tracks the least-recently-used active connection (based on keystroke and mouse motion reports).
  - When all 8 slots are filled and an eligible bonded or newly-paired peripheral connects, the oldest idle connection is cleanly disconnected to make room while preserving its bond in flash.
- **VIAL Dynamic Keymapping (WebHID):**
  - Exposes standard Vendor RawHID interface (`0xFF60:0x0061`).
  - Open [https://vial.rocks/](https://vial.rocks/) in Chrome to configure 4 layers, macros, tap-dance, and key remappings in real-time.
  - Keymaps are persisted across reboots in RP2350 flash memory.
- **Reverse Lock LED Sync:**
  - Forward CapsLock, NumLock, and ScrollLock status from the host PC back over BLE to connected keyboards.
- **Pairing Mode LED Indicator:**
  - Onboard wireless LED rapid-blinks at 5 Hz (100 ms on, 100 ms off) during active BLE pairing mode; remains completely OFF otherwise.

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
- `bonds`: Dumps bonded peripheral database and cache (BLE bonds, then classic bonds as `[c0]`, `[c1]`, ...)
- `devices`: Lists connected BLE slots with connection handles and HIDS CIDs, then connected classic slots. For each classic slot it also prints the mouse-report statistics collected since the previous `devices` call (reports/s, longest gap, histogram of gaps between reports) and whether the Pico is master or slave on the link, which is how trackpad stutter is diagnosed. The counters are reset by every call
- `desc` / `descriptor`: Dumps stored BLE HID report descriptors
- `mousespeed [<percent>] | [<slot> <percent>]`: Get or set mouse speed scaling percentage (e.g. `mousespeed 50` for 50% speed, `mousespeed 0 75` for slot 0)
- `notif [slot]`, `getreport <slot> [id]`, `getmode <slot>`, `mode <slot> <0|1>`, `suspend <slot>`: HID-over-GATT diagnostics (re-enable notifications, read an input report, read/write Protocol Mode, send Exit Suspend)
- `connparam <slot> <lat> [ms]`, `leds [mask]`: Request LL connection parameters (slave latency, interval) or view/set host Lock LED states
- `authreq [legacy|sc] [mitm|nomitm]`: Shows or sets the pairing policy used for *new* pairings (default `legacy nomitm`, see below)
- `log on|off`: Verbose mode, off by default: BTstack's internal `log_info` output (SM pairing method, GATT timeouts and security errors, HIDS client steps) plus advertising reports of non-HID devices nearby
- `hcilog on|off`, `reports on|off`: Raw ATT packet dump (ACL only) and per-report/per-keystroke dump of incoming HID reports, both off by default because every console line blocks the BTstack context for milliseconds and shows up as input latency
- `disconnect <slot>`, `unbond <idx>`, `clearbonds`: Drop a BLE link, forget one BLE bond, or forget all bonds (BLE and classic)
- `cconnect <n>`, `cdisconnect <slot>`: Bluetooth Classic only. `cconnect` makes the Pico page the classic bond `[cN]` listed by `bonds` (a host-initiated reconnect, for devices that do not reconnect by themselves); `cdisconnect` drops classic slot `<slot>`
- `cqos <slot> [type] [latency_us]`: Bluetooth Classic only, experimental. Requests a QoS (poll interval) setting on a classic slot's link, `type` 0 = no traffic, 1 = best effort, 2 = guaranteed (default 1). A latency bound of 5000 µs is already applied automatically to every classic link
- `reset`: Clears all bonded devices and resets keymap to default
- `help`: Lists all console commands

### Input latency
- Peripherals negotiate their own connection interval (7.5–8.75 ms) but ask for slave latency ~30, which lets them skip up to 30 connection events. With `BLE_ZERO_SLAVE_LATENCY` (default on in `config.h`) the Pico enforces the shortest connection interval observed on that link with latency 0 about 10 s after the peripheral's last parameter change (peripherals defend their own parameters right after connecting), preventing interval drift (e.g. from 8.75 ms to 11.25 ms); `connparam <slot> <latency> [interval_ms]` changes it at runtime and `devices` shows the current and minimum values.
  > **Warning:** this overrides what the peripheral asked for. Verified only with the Keychron Nape Pro (accepts it immediately) and the ProtoArc XK01 (re-asserts latency 32 for the first seconds after connecting, then accepts the delayed request). Other devices may drain their battery much faster at latency 0, re-negotiate repeatedly, or in the worst case drop the link right after the update. If a device misbehaves, set `BLE_ZERO_SLAVE_LATENCY` to `0` in `config.h` and rebuild, or test first with `connparam <slot> 0` on the console.
- Console output from the BTstack context blocks on the UART, so all per-event dumps are opt-in (`reports`, `log`, `hcilog`).
- The OLED is redrawn only when its content changes: `SSD1306::show()` is a ~25 ms blocking I2C transfer and TinyUSB releases the HID endpoint only from `tud_task()` in the main loop.
- Remove bonds of devices that no longer exist (`bonds`, `unbond <idx>`): background scanning runs whenever a bonded device is absent and competes with the links for radio time.

### CCCD discovery: Read By Type (`ENABLE_GATT_LEGACY_CCC_DISCOVERY`)
BTstack's default Find-Information walk for locating a report's Client Characteristic Configuration Descriptor loses the CCCD write when a peripheral returns one descriptor per response (small ATT MTU, e.g. the ProtoArc XK01 at MTU 23): every notification enable then ends in a 30 s GATT timeout and only reports whose CCCD the device restored from its own bond ever arrive. `btstack_config.h` therefore selects the Read-By-Type lookup. If a device connects but some of its reports stay silent, check `log on` output for `GATT client timeout` and `hcilog on` for missing ATT Write Requests (`12 <handle> 01 00`).

### Bluetooth Classic devices
Classic-only peripherals never advertise over BLE, so the BLE scan cannot see them. `ClassicHidHost` (`src/classic_hid_host.cpp`) handles them with BTstack's `hid_host`:
- **Pairing:** during pairing mode (`pair` / button) the Pico runs repeating ~10 s classic inquiry bursts and connects to the first peripheral-class device it finds (log lines `Found '<name>' …`; `Inquiry: ignoring …` for other classic devices in range). Put the device in its own pairing mode first; if the first attempt fails, the next burst retries. Pairing uses Secure Simple Pairing (just works, or a passkey shown on the OLED for keyboards) and falls back to the legacy PIN `0000`. Pairing from devices that were not found during pairing mode is refused.
- **Re-pairing a bonded device:** works the same way. If the device forgot its key, authentication fails, BTstack drops the Pico's stale key and a following attempt pairs afresh.
- **Reconnecting:** the Pico is always connectable and accepts incoming connections from bonded devices, so a bonded device normally reconnects when its user presses a key. Link keys are stored in the flash TLV and survive reboots. If a device does not come back by itself, use `cconnect <n>` (index from `bonds`). `unbond` only affects BLE bonds; `clearbonds` / `reset` and the 8 s button press clear classic bonds too.
- **Reports:** the HID report descriptor is parsed to find the keyboard, mouse and LED reports (any axis width, report IDs supported); reports that arrive before the descriptor is known are dropped, since their report IDs would otherwise be misread as keyboard data. Host Caps/Num Lock state is mirrored to classic keyboards.
- **Slots:** classic devices use multiplexer device indices `MAX_BLE_DEVICES` and up, so `MAX_KEYBOARDS` / `MAX_MICE` cover both.
- **Trackpad smoothness:** the Pico requests master role on every classic link and a 5 ms QoS latency bound (`LINK_QOS_LATENCY_US`), which turned reports arriving in 3–4 report bursts every ~40 ms into a steady ~10 ms cadence. Known limitation: while BLE devices are connected as well, classic reports occasionally stall for 60–300 ms because the single CYW43 radio is shared, and roughly 10 % of gaps stay at 30–60 ms.

### Pairing policy: LE legacy pairing by default
The firmware offers **LE legacy pairing without MITM** in its SMP Pairing Request by default. LE Secure Connections is compiled in but opt-in (`authreq sc`), because the ProtoArc XK01 keyboard pairs and encrypts fine over Secure Connections yet never sends a single HID input notification afterwards, while it works with legacy pairing. A keyboard that insists on MITM still gets passkey entry (the PIN shows on the OLED), and `authreq legacy mitm` forces it. The policy only affects new pairings: `unbond <idx>` a device and re-pair it to apply a new policy.

---

## How to Pair a Device

(For classic-only keyboards, keypads and mice the same steps apply; see [Bluetooth Classic devices](#bluetooth-classic-devices).)

1. **Enter Pairing Mode:**
   - Press the push button on `GP6` (or hold for 2s, or run `pair` from console) until the OLED screen displays `BLE PAIRING...` (or toast `Pairing Mode (60s)`).
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
