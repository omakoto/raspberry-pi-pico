# ESP32-S3 BLE HID Multiplexer

A Bluetooth Low Energy (BLE) Human Interface Device (HID) multiplexer and dynamic keymap engine for the
**Seeed Studio XIAO ESP32-S3** and the **ESP32-S3-DevKitC-1**, ported from
[`pico/bt-hid-multiplexer`](../../pico/bt-hid-multiplexer/) (Raspberry Pi Pico W / Pico 2 W).

It connects up to 8 wireless BLE keyboards, mice and trackpads and merges them into a **single composite
USB HID device** (keyboard + mouse + VIAL WebHID) on your computer. See [DESIGN.md](DESIGN.md) for how
the port is built.

**BLE only:** the ESP32-S3 radio supports Bluetooth LE but not Bluetooth Classic (BR/EDR), so the Pico
version's classic HID host (keyboards and mice that only speak classic Bluetooth) is not available here.

---

## Features

- **BLE Central / HOGP host:** keyboards, mice and keyboard+trackpad combos (HID over GATT), up to 8 at
  once (`MAX_BLE_DEVICES`), with the least recently used link dropped when a ninth bonded device shows
  up. Supports 8-bit, 12-bit packed (Logitech) and 16-bit mouse reports, vertical and horizontal wheel,
  and 5 buttons.
- **Configurable mouse speed** (`mousespeed`), globally or per device, without losing small movements.
- **Multi-device rollover:** modifiers and keys of all keyboards are merged without stuck keys.
- **Bonds survive reboots** (BTstack's bond database in NVS). Background scanning only reconnects bonded
  devices; new devices are only accepted during pairing mode.
- **VIAL keymapping (WebHID):** remap every key, modifier, mouse button and mouse movement across 8
  layers at [vial.rocks](https://vial.rocks/); per-device layers, set in VIAL's Layout tab (one
  dropdown per paired device, by name, see
  [manual/per-device-mapping.md](manual/per-device-mapping.md)). Saved in NVS.
- **Chord mode for shortcut keypads:** the XP-Pen ACK05 Mini Keydial sends fixed shortcuts (Ctrl+Z,
  Ctrl+Shift+Z, a bare Ctrl, ...) that share keys and cannot be remapped one by one. The multiplexer
  turns it into a numeric keypad instead: dial left/right = Keypad `-`/`+`, the dial button = Keypad
  Enter, and keys 1-10 (top left to bottom right) = Keypad `1`-`9`, `0`. Each is a key of its own
  that VIAL can remap, e.g. in the pad's per-device layer. With Num Lock off, the host reads
  Keypad `1`-`9`/`0` as navigation keys. See [manual/xppen-ack05.md](manual/xppen-ack05.md).
- **Reverse Lock LED sync:** the host's Caps/Num/Scroll Lock state is sent to the connected keyboards.
- **SSD1306 or SH1106 OLED (128x64, I2C):** connection state, device name, the layer of the device used last, and
  the 6-digit passkey when a keyboard needs one. It goes dark a minute after the last change or input and
  comes back on with the next one. Optional: the firmware runs without a display. Set
  `OLED_CONTROLLER` in `main/config.h` to the module's controller (default: SH1106).
- **Pairing button.**
- **Serial console** on UART0 (and optionally on the USB serial port) with diagnostics.
- **Hang recovery:** a task watchdog reboots a hung board in 5 s and the previous run's log survives.

---

## Hardware

### Boards

| | XIAO ESP32-S3 | ESP32-S3-DevKitC-1 |
| :--- | :--- | :--- |
| USB | One USB-C (native USB) | `USB` port (native) and `UART` port (USB-UART bridge) |
| Console | USB-UART adapter on `D6`/`D7`, or the USB serial port | The `UART` USB-C port |

The same firmware image runs on both boards: the external parts use the same GPIOs, and the boards'
own LEDs are not used. Plug the **native USB port** (`USB` on the DevKitC)
into the computer that should receive the keyboard and mouse input.

### Wiring

| Component | Signal | GPIO | XIAO pin | DevKitC header | Notes |
| :--- | :--- | :--- | :--- | :--- | :--- |
| OLED | SDA | `GPIO5` | `D4` | Row B, silk `5` | I2C, 400 kHz |
| OLED | SCL | `GPIO6` | `D5` | Row B, silk `6` | |
| OLED | VCC / GND | `3V3` / `GND` | `3V3` / `GND` | `3V3` / `G` | |
| Push button | to GND | `GPIO4` | `D3` | Row B, silk `4` | Active LOW, internal pull-up |
| USB serial enable | jumper to GND | `GPIO7` | `D8` | Row B, silk `7` | Read once at boot, see below |
| Console UART0 | TX / RX | `GPIO43` / `GPIO44` | `D6` / `D7` | (on-board bridge) | 115200 8N1 |

#### OLED pull-ups

I2C needs pull-up resistors on SDA and SCL. The firmware enables the ESP32's internal pull-ups, but
they are weak (about 45 kΩ), so it relies on the module's own. Most 0.96" SSD1306 modules have
4.7-10 kΩ pull-ups. Some 1.3" SH1106 modules (e.g. the Hosyond 1.3" module) have weak ones or none.
A display without them may work at 400 kHz but is marginal: it can glitch or stay blank. Add
**4.7 kΩ from SDA to 3V3 and from SCL to 3V3** if that happens.

`i2cscan` on the [serial console](#serial-console) shows whether the bus is healthy. A healthy bus
lists only the display (`0x3C`). With weak pull-ups SDA rises too slowly, and the scan also lists
other, mostly even addresses (e.g. `0x28 0x3C 0x4E 0x74`), because a line still on its way up after
the address byte reads as an ACK. If nothing answers at an odd address and every even address
"answers", the module is probably not connected or not powered: check VCC at the module and the SDA
and SCL wires.

Free pins for later: `GPIO1`, `GPIO2`, `GPIO8`, `GPIO9` (XIAO `D0`, `D1`, `D9`, `D10`) and `GPIO3`
(`D2`, a strapping pin). The on-board BOOT button is not used by the firmware; it is how to force
download mode by hand.

---

## Building and Flashing

Requires ESP-IDF v5.3 (`~/esp-idf`, or `IDF_PATH`) and BTstack, which is compiled from
`~/pico-sdk/lib/btstack` (or `BTSTACK_ROOT`), the same BTstack the Pico build uses.

```bash
./00-build.sh                # build (the same image for both boards)
./01-install.sh              # flash
./02-monitor.sh [port]       # serial console
```

`01-install.sh` picks the way to flash by itself:
- **Normally:** it asks the running firmware to reboot into the ROM download mode through the VIAL
  interface (or the USB serial port), then flashes over the native USB port. This works the same on
  both boards.
- **If that fails and a USB-UART bridge is connected** (the DevKitC's `UART` port): through the bridge,
  which esptool resets into download mode by itself. `-p <port>` picks a port explicitly.
- **First flash, or the firmware does not respond:** hold BOOT, tap RESET, release BOOT, then run
  `./01-install.sh` again.

Both scripts take `-h`.

### Serial console

The UART0 console is always on: on the DevKitC it is the `UART` USB-C port, on the XIAO it needs a
3.3 V USB-UART adapter on `D6` (TX) / `D7` (RX).

**The board's own USB serial port is off by default,** so the board appears to the computer as just the
keyboard/mouse and VIAL interfaces. To add it, connect `GPIO7` (XIAO `D8`) to GND *before* plugging the
board in, or build with `./00-build.sh -D USB_SERIAL_ALWAYS=ON` (`-D USB_SERIAL_ALWAYS=OFF` to go back).
A 1200-baud "touch" on that port reboots into download mode.

All console output goes through an 8 KB ring buffer that never blocks Bluetooth or USB; if output comes
faster than the console can take it, the oldest text is dropped with a `[log: N bytes dropped]` note.

Commands:
- `pair` / `scan`: 60-second pairing window; `stop`: stop pairing and scanning
- `status`, `devices`, `bonds`, `desc`: connection state, connected devices, bonds, HID descriptors
- `mousespeed [<percent>] | [<slot> <percent>]`: mouse speed scaling
- `devlayer` (`dl`) `[<layer> | <dev> <layer> | clear [<dev>] | list]`: per-device layers, the same
  as VIAL's Layout tab, see [Per-device layers from the console](#per-device-layers-from-the-console)
- `notif [slot]`, `getreport <slot> [id]`, `getmode <slot>`, `mode <slot> <0|1>`, `suspend <slot>`:
  HID-over-GATT diagnostics
- `connparam <slot> <lat> [ms]`, `leds [mask]`: request LL connection parameters; show/set the host
  Lock LED state
- `authreq [legacy|sc] [mitm|nomitm]`: pairing method tried first for new pairings (default
  `legacy nomitm`; a failed attempt switches to the other method, see below)
- `log on|off`, `hcilog on|off`, `reports on|off`: BTstack log, raw HCI dump (commands, events and
  ACL data such as SMP and ATT, without advertising reports), per-report dump
  (the latter two switch themselves off after 15 s)
- `disconnect <slot>`, `unbond <idx>`, `clearbonds`
- `i2cscan`: list the addresses that answer on the OLED's I2C bus (for a display that stays blank)
- `resetkeymap`: reset the VIAL keymap, keeping the bonds
- `reset`: factory reset (bonds, keymap, per-device bindings and macros)
- `lastlog`: the previous run's log; `hangtest`: hang on purpose to test the watchdog
- `bootloader`: reboot into download mode; `reboot`: restart
- `help`

### Per-device layers from the console

[manual/per-device-mapping.md](manual/per-device-mapping.md) binds devices to layers in VIAL's Layout
tab. `devlayer` (alias `dl`) does the same from the console. It refers to connected devices by their
index, or binds **the device that sent input most recently**, so you don't have to name the device:

| Command | Effect |
| --- | --- |
| `devlayer <layer>` | Bind the device used last to `<layer>` (1–7); `0` removes its binding |
| `devlayer <dev> <layer>` | Bind connected device `<dev>` |
| `devlayer clear [<dev>]` | Remove the binding of the device used last, or of `<dev>` |
| `devlayer list` (or just `devlayer`) | Connected devices with index, name, address and bound layer; the saved bindings; the device used last |

To bind a device without knowing its index, use only that device (e.g. scroll its wheel once), then
type `devlayer 3` right away:

```
Device 2 bound to layer 3. Edit that layer in VIAL.
```

- `No device has sent input yet`: the multiplexer only remembers input since it booted or since that
  device reconnected. Move or click the device and try again.
- If another device sent input in between, the wrong one gets bound: `devlayer clear`, then try again
  without touching anything else.
- `devlayer list` also shows the bindings of devices that are no longer paired, marked `(unpaired)`.
  Remove those in VIAL's Layout tab (their `Unpaired` checkboxes).
- `resetkeymap` resets the keymap and keeps the bindings; `reset` removes them, together with the
  bonds, the keymap and the macros.

### Hang recovery and the previous run's log

- **Task watchdog.** All Bluetooth, USB-report and console-command work runs on one task (`bt_app`).
  If it makes no progress for 5 s (which also covers a hung Bluetooth stack), the task watchdog reboots
  the board. `hangtest` hangs it on purpose.
- **The previous run's log survives** a watchdog, panic or software reset (it lives in RAM that startup
  does not clear), together with a trail of breadcrumbs: the last Bluetooth, storage and pairing events
  and the stage `bt_app` was in. After a crash the console prints a summary; read the full log with
  `lastlog`, or without a serial port with `./lastlog.py` (`--info` for the summary only). A power cycle
  clears it.
  - Breadcrumb codes: `0x1xxx` HCI event (`0x11xx` LE meta), `0x2xxx` security manager, `0x3xxx`
    HID-over-GATT, `0x5001`/`0x5002` keymap write begin/end, `0x5003`/`0x5004` bindings write,
    `0x6001`/`0x6002` pairing started/stopped.
  - Stages: 0 idle, 1 USB event, 2 console command, 3 report flush, 4 button, 5 timer, 6 UI update,
    7 VIAL request, 8 keymap save, 99 `hangtest`.

---

## How to Pair a Device

1. **Pairing mode:** press the button (or hold it 2 s, or run `pair`). The OLED shows `BT PAIRING...`.
2. **Put the keyboard or mouse in its pairing mode.**
3. **Passkey:** a keyboard that asks for one shows it on the OLED (and the console); type it on that
   keyboard and press Enter.
4. **Connected:** the OLED shows `BT: <name>` and the input goes to the computer.

Holding the button for 8 s clears all bonds and resets the keymap (the per-device bindings stay, and
apply again when the devices are paired again; the `reset` console command clears those as well).

Up to 8 devices stay paired. Pairing a ninth removes the pairing of the device used least recently
(preferring one that is not connected); its per-device binding is kept, see
[manual/per-device-mapping.md](manual/per-device-mapping.md#devices-that-are-no-longer-paired).

### Pairing policy: LE legacy pairing first, Secure Connections next
The firmware offers **LE legacy pairing without MITM** first. If a pairing attempt fails (the device
rejects it or just drops the link), the next attempt offers **LE Secure Connections**, and so on
alternately until a pairing succeeds (also across pairing windows, for devices that leave pairing mode
after one failure: just put the device in pairing mode again), because devices differ: the ProtoArc XK01
keyboard pairs and encrypts fine over Secure Connections yet never sends a single HID input notification
afterwards, so it needs legacy pairing, while the Keychron Nape Pro on one of its host slots accepts only
Secure Connections and drops the link when offered legacy pairing. The console shows which method each
attempt offers. `authreq [legacy|sc] [mitm|nomitm]` sets the method tried first (default `legacy
nomitm`); a keyboard that insists on MITM still gets passkey entry (the PIN shows on the OLED), and
`authreq legacy mitm` forces it. The policy only affects new pairings: `unbond <idx>` a device and
re-pair it to apply a new policy.

### CCCD discovery: Read By Type (`ENABLE_GATT_LEGACY_CCC_DISCOVERY`)

BTstack's default Find-Information walk for locating a report's Client Characteristic Configuration
Descriptor loses the CCCD write when a peripheral returns one descriptor per response (small ATT MTU,
e.g. the ProtoArc XK01 at MTU 23): every notification enable then ends in a 30 s GATT timeout.
`components/btstack/include/btstack_config.h` therefore selects the Read-By-Type lookup. If a device
connects but some of its reports stay silent, check `log on` output for `GATT client timeout`.

### Input latency

- With `BLE_ZERO_SLAVE_LATENCY` (on by default in `main/config.h`) the firmware asks each peripheral
  for slave latency 0 at the shortest interval it used, about 10 s after the peripheral's last parameter
  change. **Warning:** this overrides what the peripheral asked for and costs it battery; verified only
  with the Keychron Nape Pro and the ProtoArc XK01. Set it to `0` if a device misbehaves, or try
  `connparam <slot> 0` first.
- The OLED is drawn by its own task, so a display update no longer delays HID reports.
- Remove bonds of devices that no longer exist (`bonds`, `unbond <idx>`): background scanning runs
  whenever a bonded device is absent and competes with the links for radio time.

---

## Keymapping with VIAL

1. On Linux, run `~/cbin/setup/config-hidraw-permission` once so Chrome may open the board's VIAL hidraw
   node (it covers the board's USB ID `045e:4004`, and Espressif's vendor ID `303a`). Re-plug the board afterwards.
2. Open **[https://vial.rocks/](https://vial.rocks/)** in Chrome or another Chromium-based browser, click
   **Start** and select **ESP32-S3 BLE HID Multiplexer**.
3. Remap keys across 8 layers. Changes take effect at once and are saved half a second after the last
   edit.
4. **Per-device layers:** the **Layout** tab has one dropdown per paired device, labelled with its
   name: pick a layer to remap that device on its own, see
   [manual/per-device-mapping.md](manual/per-device-mapping.md). Bindings of devices that are no
   longer paired follow as `Unpaired` checkboxes; clear one to remove that binding. The firmware
   builds the VIAL keyboard definition at runtime for this (`main/vial_definition.cpp`), so reload
   vial.rocks after pairing.

The device appears as a Microsoft device, `045e:4004`: Microsoft's vendor ID with esp_tinyusb's
generic HID product ID. No Microsoft product uses this ID, so no driver quirks or vendor software
attach to it. To use Espressif's vendor ID instead (`303a:4004`), build with
`./00-build.sh -D USB_ID_MICROSOFT=OFF` (`-D USB_ID_MICROSOFT=ON` to go back). The product ID is
shared with other TinyUSB gadgets, so the scripts find the board by its VIAL interface (usage page
`0xFF60`) as well.

### Layers and what is supported

- **Keys:** the matrix is 16×16, one cell per HID keyboard usage, so every key and modifier can be
  remapped. Keys left `Transparent` on an upper layer use the next lower layer that is switched on
  (layers no layer key selected are skipped), and finally layer 0; for a device bound to a layer, its
  own layer comes just before layer 0.
- **Keycodes that work:** basic keys, modifiers, modifier-wrapped keys such as `LSFT(KC_A)`,
  `MO`/`TG`/`TO`/`DF` layer keys (in the numbering of the VIAL protocol version the firmware reports),
  `KC_NO`, `KC_TRNS`, mouse buttons 1–5, the mouse wheel/cursor keycodes, `KC_ACL0`–`KC_ACL2`,
  media and browser keys (volume, play/pause, next/previous track, browser back/forward/refresh/home,
  calculator, mail, brightness and so on; one at a time), Power/Sleep/Wake, and macros `M0`–`M63`.
  Media, browser and system keys do nothing in the BIOS / boot menu, which only reads keyboard keys.
- **Mouse keys:** a key mapped to a cursor or wheel keycode works as in QMK (accelerated mode, QMK's
  default settings): it moves or scrolls once when pressed, and after 100 ms keeps going while held,
  faster the longer it is held (the cursor reaches full speed after about half a second, the wheel
  after about 3 s). Holding a key mapped to `KC_ACL0`, `KC_ACL1` or `KC_ACL2` sets a fixed slow,
  medium or fast speed instead while it is held.
- **Layer keys and per-device layers:** a layer key only switches layers for its own group of devices.
  All unbound devices are one group; the devices bound to the same layer are another. So a layer key
  on a bound device does not change what any other device does, except devices bound to the same layer.
- **Macros:** edit them in VIAL's **Macros** tab (64 macros, 4096 bytes in total) and put `M0`…`M63`
  on a key or mouse button. A macro can type text (ASCII, US layout; other characters are skipped),
  tap, press and release keys (including modifier-wrapped ones) and wait. One macro plays at a time;
  keys a macro leaves pressed are released when it ends. Macros are saved half a second after the
  last edit; the `reset` console command clears them.
- **Not supported:** tap dance,
  mod-tap/layer-tap, combos; the 6-key rollover limit applies.
- **Mouse:** the bottom row of the Vial key map is the mouse: buttons 1–8 on the left (6–8 default to
  F13–F15), movement directions on the right (cursor up/down/left/right, wheel up/down/left/right).
  Map a button to `MO(1)` and, on layer 1, cursor up/down to `KC_WH_U`/`KC_WH_D` to scroll by moving the
  mouse while the button is held; map a movement to a key (e.g. `KC_VOLU`) to tap it once per wheel
  notch. `MOUSE_COUNTS_PER_WHEEL_NOTCH` in `main/config.h` sets how much movement is one notch.
- **Changing the key list:** edit `gen-vial-layout.py`, run it, and rebuild.

---

## Tests

`test/run-host-test.sh` builds the keymap/multiplexer logic, the macros, the log ring, the VIAL
definition, the bonded device table, the stored bindings format and the HID report descriptor parser
with the host compiler and runs their tests. Run it after touching `main/multiplexer.cpp`,
`main/macros.cpp`, `main/virtual_matrix.cpp`, `main/device_bindings.cpp`, `main/log_ring.cpp`,
`main/vial_definition.cpp`, `main/bond_table.cpp`, `main/bindings_format.cpp`,
`main/hid_descriptor.cpp`, `main/chord_mode.cpp`, `main/storage.h`, `main/config.h` or `gen-vial-layout.py`.

`test/run-host-test.sh --coverage` also prints the line coverage of each of those sources and keeps
gcov's annotated copies, with the lines that never ran marked `#####`.
