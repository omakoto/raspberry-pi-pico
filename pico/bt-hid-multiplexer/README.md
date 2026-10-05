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
  - Displays connection status, connected peripheral names, the keymap layer of the device used last (its bound layer, or the layer a layer key selected), and USB connection health.
  - **Pairing Passkey Display:** When pairing a keyboard requiring Secure Simple Pairing / Passkey Entry, the 6-digit PIN is displayed prominently on the OLED (`Type 123456 + Enter on keyboard`).
- **Push Button Pairing Controller:**
  - Short press: Toggle pairing mode on/off (60-second discovery window).
  - Long press (2s): Start pairing mode (60-second discovery window).
  - Extra long press (8s): Factory reset (wipes all bonded BLE devices and resets keymap to default).
  - **Non-Aggressive Pairing:** Background scanning strictly reconnects to already-bonded peripherals and will never hijack unbonded devices advertising in pairing mode nearby. New devices are only discovered and paired during an active Pairing Mode window.
- **LRU Active Connection Eviction (Up to 8 Concurrent Devices):**
  - Supports up to 8 active BLE connections simultaneously (`MAX_BLE_DEVICES = 8`).
  - Tracks the least-recently-used active connection (based on keystroke and mouse motion reports).
  - When all 8 slots are filled and an eligible bonded or newly-paired peripheral connects, the oldest idle connection is cleanly disconnected to make room while preserving its bond in flash.
- **VIAL Dynamic Keymapping (WebHID):**
  - Exposes standard Vendor RawHID interface (`0xFF60:0x0061`).
  - Open [https://vial.rocks/](https://vial.rocks/) in Chrome to remap every key (including modifiers, F-keys, arrows, the numpad and international keys) and the mouse buttons and motion across 8 layers in real-time. See [Keymapping with VIAL](#keymapping-with-vial) for what is supported.
  - Keymaps are persisted across reboots in RP2350 flash memory.
- **Reverse Lock LED Sync:**
  - Forward CapsLock, NumLock, and ScrollLock status from the host PC back over BLE to connected keyboards.
- **Pairing Mode LED Indicator:**
  - Onboard wireless LED rapid-blinks at 5 Hz (100 ms on, 100 ms off) during active pairing mode; remains completely OFF otherwise.

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
| **USB serial enable** | Jumper to GND | `GP10` | Pin 14 | Connect to GND (e.g. pin 13 or 8) before plugging in to add the USB serial port; internal pull-up, read once at boot |
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
Interactive serial console and logger running concurrently on USB CDC (`/dev/ttyACM*`) and hardware UART0 (GP16/GP17 at 115,200 baud).

**The USB serial port is off by default**, so the board does not add a serial port to your computer while you work on other microcontroller projects; it then enumerates as just the keyboard/mouse and VIAL interfaces. To get it, connect `GP10` (pin 14) to GND *before* powering the board (the pin is read once at boot; unplug and re-plug after changing the jumper). For "off", leave `GP10` open: do not tie it to another GPIO, because unused pins have an internal pull-down that fights `GP10`'s pull-up and makes the reading unreliable. The hardware UART console is always available. `01-install.sh` does not need the serial port: without it, it reboots the board into BOOTSEL through the VIAL interface (VIA "jump to bootloader" command). For development there is also a build option that always enables the port: `./00-build.sh -DUSB_SERIAL_ALWAYS=ON` (use `-DUSB_SERIAL_ALWAYS=OFF` to go back).

```bash
./02-monitor.sh
```
Commands supported on either console:
- `bootloader` / `bootsel`: Drops board directly into USB BOOTSEL ROM for updates
- `pair` / `scan`: Initiates 60-second BLE discovery pairing window
- `stop`: Halts BLE scanning
- `status`: Displays uptime, connection state, device name, and the layer of the device used last
- `bonds`: Dumps bonded peripheral database and cache (BLE bonds, then classic bonds as `[c0]`, `[c1]`, ...)
- `devices`: Lists connected BLE slots with connection handles and HIDS CIDs, then connected classic slots. For each classic slot it also prints the mouse-report statistics collected since the previous `devices` call (reports/s, longest gap, histogram of gaps between reports) and whether the Pico is master or slave on the link, which is how trackpad stutter is diagnosed. The counters are reset by every call
- `desc` / `descriptor`: Dumps stored BLE HID report descriptors
- `mousespeed [<percent>] | [<slot> <percent>]`: Get or set mouse speed scaling percentage (e.g. `mousespeed 50` for 50% speed, `mousespeed 0 75` for slot 0)
- `notif [slot]`, `getreport <slot> [id]`, `getmode <slot>`, `mode <slot> <0|1>`, `suspend <slot>`: HID-over-GATT diagnostics (re-enable notifications, read an input report, read/write Protocol Mode, send Exit Suspend)
- `connparam <slot> <lat> [ms]`, `leds [mask]`: Request LL connection parameters (slave latency, interval) or view/set host Lock LED states
- `authreq [legacy|sc] [mitm|nomitm]`: Shows or sets the pairing policy used for *new* pairings (default `legacy nomitm`, see below)
- `log on|off`: Verbose mode, off by default: BTstack's internal `log_info` output (SM pairing method, GATT timeouts and security errors, HIDS client steps) plus advertising reports of non-HID devices nearby
- `hcilog on|off`, `reports on|off`: (switch themselves off after 15 s) Raw ATT packet dump (ACL only) and per-report/per-keystroke dump of incoming HID reports, both off by default because every console line blocks the BTstack context for milliseconds and shows up as input latency
- `disconnect <slot>`, `unbond <idx>`, `clearbonds`: Drop a BLE link, forget one BLE bond, or forget all bonds (BLE and classic)
- `cconnect <n>`, `cdisconnect <slot>`: Bluetooth Classic only. `cconnect` makes the Pico page the classic bond `[cN]` listed by `bonds` (a host-initiated reconnect, for devices that do not reconnect by themselves); `cdisconnect` drops classic slot `<slot>`
- `cqos <slot> [type] [latency_us]`: Bluetooth Classic only, experimental. Requests a QoS (poll interval) setting on a classic slot's link, `type` 0 = no traffic, 1 = best effort, 2 = guaranteed (default 1). A latency bound of 5000 µs is already applied automatically to every classic link
- `devlayer` (alias `dl`) `[<layer> | <dev> <layer> | clear [<dev>] | list]`: Binds a device to a keymap layer so it can be remapped on its own, see [manual/per-device-mapping.md](manual/per-device-mapping.md)
- `resetkeymap`: Resets the VIAL keymap (all layers) to the defaults and keeps all bonds
- `lastlog`: Shows the log and last events of the previous run (kept across a watchdog reset), see [Hang recovery](#hang-recovery-and-the-previous-runs-log)
- `hangtest`: Hangs the main loop on purpose to test the watchdog recovery
- `reset`: Clears all bonded devices and per-device layer bindings and resets keymap to default
- `help`: Lists all console commands

### Hang recovery and the previous run's log
- **Console output never blocks.** `printf` and the console only append to an 8 KB ring buffer (`src/log_ring.cpp`); the main loop drains it to the USB serial port and the UART without waiting. A slow or absent console can no longer stall Bluetooth or USB, and the two contexts no longer write to the USB serial FIFO at the same time (which used to garble output). If output arrives faster than it can be sent, the oldest text is dropped and a `[log: N bytes dropped]` line says so. A terminal that opens the USB serial port first receives the recent backlog.
- **Watchdog.** If the main loop, or the Bluetooth stack's run loop (checked through a 1 s heartbeat timer), makes no progress for 5 s, the hardware watchdog reboots the board. The outage is 1-2 s instead of "unplug it". `hangtest` hangs the main loop on purpose to try this.
- **The previous run's log survives that reboot** (the ring lives in RAM the watchdog reset does not clear), together with a trail of *breadcrumbs*: the last Bluetooth, flash and pairing events and the main loop stage where it stopped. After a watchdog reboot the console prints a summary. Read the full log with the `lastlog` console command, or without any serial port by running `./lastlog.py` (`--info` for the summary only), which uses the VIAL interface. Do it before unplugging the board: a power cycle clears it.
  - Breadcrumb codes: `0x1xxx` HCI event (`0x11xx` LE meta), `0x2xxx` security manager, `0x3xxx` HID-over-GATT, `0x4xxx` classic HID, `0x5001`/`0x5002` keymap flash write begin/end, `0x5003`/`0x5004` bindings flash write, `0x6001`/`0x6002` pairing started/stopped. The main loop stages are 1 USB, 2 console, 3 report flush, 4 button, 5 LED and scan, 6 display; 99 is `hangtest`.
- **Verbose dumps switch themselves off.** `reports on`, `hcilog on` and `log on` turn off again after 15 s, and the raw report dump prints at most 40 lines per second, so a forgotten dump cannot flood the console or fill the log ring.

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

### Pairing policy: LE legacy pairing first, Secure Connections next
The firmware offers **LE legacy pairing without MITM** first. If a pairing attempt fails (the device rejects it or just drops the link), the next attempt in the same pairing window offers **LE Secure Connections**, and so on alternately, because devices differ: the ProtoArc XK01 keyboard pairs and encrypts fine over Secure Connections yet never sends a single HID input notification afterwards, so it needs legacy pairing, while the Keychron Nape Pro on one of its host slots accepts only Secure Connections and drops the link when offered legacy pairing. The console shows which method each attempt offers. `authreq [legacy|sc] [mitm|nomitm]` sets the method tried first (default `legacy nomitm`); a keyboard that insists on MITM still gets passkey entry (the PIN shows on the OLED), and `authreq legacy mitm` forces it. The policy only affects new pairings: `unbond <idx>` a device and re-pair it to apply a new policy.

## How to Pair a Device

(For classic-only keyboards, keypads and mice the same steps apply; see [Bluetooth Classic devices](#bluetooth-classic-devices).)

1. **Enter Pairing Mode:**
   - Press the push button on `GP6` (or hold for 2s, or run `pair` from console) until the OLED screen displays `BT PAIRING...` (or toast `Pairing Mode (60s)`).
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

1. On Linux, run `~/cbin/setup/config-hidraw-permission` once (idempotent) so Chrome may open the board's VIAL hidraw node. It installs a udev rule for all Raspberry Pi USB devices (vendor `2e8a`: Pico, Pico W, Pico 2, Pico 2 W); without it vial.rocks hangs on "Connecting to the device...". Re-plug the board afterwards.
2. Connect the Pico upstream USB cable to your computer.
3. Open **[https://vial.rocks/](https://vial.rocks/)** in Google Chrome or any Chromium-based browser.
4. Click **Start** and select **Pico W BLE HID Multiplexer**.
5. Remap keys across 8 layers. Changes take effect instantly and are stored in flash.

### Layers and what is supported

- **Keys:** the matrix is 16×16, one cell per HID keyboard usage, so every key and modifier on any connected keyboard can be remapped. Keys on upper layers that are left as `Transparent` use the layer below. Only the base layer is non-transparent by default.
- **Keycodes that work:** basic keys, modifiers (`LCTL`…`RGUI`), modifier-wrapped keys such as `LSFT(KC_A)`, `MO`/`TG`/`TO`/`DF` layer keys (keycodes use the numbering of the VIAL protocol version the firmware reports, see `src/config.h`), `KC_NO`, `KC_TRNS`, mouse buttons 1–5 and the mouse wheel/cursor keycodes below.
- **Not supported:** media/consumer and system keys (the USB device has no consumer report), macros, tap dance, mod-tap/layer-tap, combos, and the 6-key rollover limit still applies.
- **Mouse:** the bottom row of the Vial key map is the mouse. The eight keys on the left are mouse buttons 1–8 (only 1–5 can be sent on to the host as mouse buttons; 6–8 default to F13–F15 and can be remapped to anything). The eight keys on the right are the directions of mouse movement in the order cursor up, down, left, right, then wheel up, down, left, right. By default they are mapped to `KC_MS_U/D/L/R` and `KC_WH_U/D/L/R`, which reproduces the physical behaviour.
  - **Remap a button:** put any key, modifier or `KC_BTN1`…`KC_BTN5` on it.
  - **Button + movement = something else:** map one mouse button to `MO(1)` on layer 0, then on layer 1 map "cursor up" to `KC_WH_U` and "cursor down" to `KC_WH_D`. While the button is held, moving the mouse up/down scrolls; `KC_MS_L/R` ↔ `KC_WH_L/R` does the same for horizontal movement. A movement can also be turned into another movement (e.g. swap or invert axes), into a key (one tap per wheel notch of movement or per real wheel notch, e.g. `KC_VOLU`/`KC_VOLD` for volume), or disabled with `KC_NO`. `KC_MUTE`/`KC_VOLU`/`KC_VOLD` are sent as the keyboard-page volume keys, which Linux sees as `KEY_MUTE`/`KEY_VOLUMEUP`/`KEY_VOLUMEDOWN`. `MOUSE_COUNTS_PER_WHEEL_NOTCH` in `src/config.h` sets how many counts of movement are one wheel notch.
- **Per device:** a Bluetooth device can be bound to a layer so that remapping applies to it alone (for example one mouse's wheel as volume): see [manual/per-device-mapping.md](manual/per-device-mapping.md).
- **Persistence:** keymap edits are written to flash about half a second after the last change.
- **Changing the key list:** edit `gen-vial-layout.py`, run it, and rebuild. `test/run-host-test.sh` tests the remapping logic on the host.

