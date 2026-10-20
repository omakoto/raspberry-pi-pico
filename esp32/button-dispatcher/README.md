# button-dispatcher (ESP32-S3)

A standalone ESP32-S3 version of the BLE button dispatcher: it turns BLE (BTHome) button presses,
such as those of Shelly BLU remotes, into HTTP/HTTPS requests and SwitchBot commands or scenes. Set
it up once from a web page; no Raspberry Pi needed.

**Status: the firmware builds and the core is tested on the PC; it has not been flashed or run on a board yet.**

This is a port of [`pi/button-dispatcher`](../../pi/button-dispatcher/README.md) (Python, Raspberry
Pi, installed and working). That README is the reference for **behavior**: the press flow, triggers
and bindings, actions, web page, security model, and the config file format. This document covers
only what is different on the ESP32-S3, so the two don't drift apart.

Hardware findings behind both (packet formats, wheel behavior):
`~/history/reports/electronics/shelly-remote-switches.md`.

## One contract, two implementations

To keep the Python and C++ versions equivalent:

- **Same web page.** The firmware embeds the files from `pi/button-dispatcher/button_dispatcher/static/`
  at build time (gzip-compressed); there is no second copy of the UI.
- **Same JSON API and config format.** A config exported from one imports into the other
  (credentials stay out of exports by default, as on the Pi).
- **Same test vectors.** The C++ unit tests use the packets captured from the real remotes, the same
  ones the Python tests use.
- **Same behavior fixes.** Dedupe remembers several recent packet ids per device (this fixes a gap in
  the Python version, which remembers only the latest; the Python code should be updated to match).

## Hardware

An ESP32-S3 board with 8 MB flash. Developed on an ESP32-S3-DevKitC-1; the target is also the Seeed
Studio XIAO ESP32-S3 (same firmware; only the optional status LED pin may differ). Bluetooth and Wi-Fi
are on-chip, so nothing needs wiring. The board is powered and set up over its USB port.

| Function | ESP32-S3 GPIO | DevKitC-1 Physical Pin | Seeed Studio XIAO Pin | Details |
| :--- | :---: | :---: | :---: | :--- |
| **Status LED** | `GPIO21` | Row A, Pin 18 (silk `21`) | Onboard Yellow LED | Active-low, optional (`led_pin = -1` disables it) |
| **Ack buzzer** | `GPIO9` | J1, between `46` and `10` (silk `9`) | `D10`, next to `3V3` | Optional (`buzzer_pin = -1` disables it); active-low by default, see below |
| **USB (config drive, serial)** | `GPIO19`/`GPIO20` | the port marked `USB` | USB-C connector | Native USB: serial console and the drag-and-drop config drive |

### Ack buzzer

An active buzzer (it has its own oscillator, so a single fixed pitch) acknowledges presses:

- When a press fires a binding: as many short beeps as clicks (single press 1, double press 2, triple press
  3; long-press variants and the wheel count like their short counterparts).
- When the action finishes: one short beep for success, three long beeps for a failure (an active buzzer
  cannot change pitch, so a failure is told apart by its length).
- Every sequence of beeps starts at least 1 s after the previous one ended, so an acknowledgement and its
  result are heard as separate things.

Wiring for a ready-made module with `GND`, `VCC` and `I/O` pins that beeps when `I/O` is shorted to `GND`
(a low-level trigger, the default): `I/O` to `GPIO9`, `VCC` to `3V3`, `GND` to `GND`. The module has its own
transistor. For a bare buzzer, drive it through an NPN transistor (GPIO through about 1 kΩ to the base,
emitter to `GND`, buzzer between `3V3` and the collector) and set `buzzer_active_low = false`.

## How it differs from the Pi version

| | Raspberry Pi | ESP32-S3 |
|---|---|---|
| Language | Python (aiohttp, bleak) | C++17 on ESP-IDF (NimBLE, esp_http_server, esp_http_client) |
| First-time setup | `./deploy.sh pi9.local` | Wi-Fi settings in `config.toml`, flashed with the firmware or edited on the USB drive, as in `wol-bridge` |
| Config and credentials | `/var/lib/button-dispatcher/` | a separate internal `state` partition (never exposed over USB) |
| Passwords | scrypt | PBKDF2-HMAC-SHA256 (mbedtls has no scrypt); not portable, never exported |
| Sessions | signed cookie, survive restarts | signed cookie with a per-boot key; a reboot signs everyone out |
| Clock | system clock | SNTP; needed for HTTPS certificates, SwitchBot request signing and log times |
| BLE scanning | active (bleak default) | passive: receive only, no scan requests |

### Layout

```
esp32/button-dispatcher/
  README.md  00-build.sh  01-install.sh  02-monitor.sh  CMakeLists.txt  partitions.csv
  sdkconfig.defaults  fatfs_data/config.toml          Wi-Fi etc. (same keys as wol-bridge)
  components/dispatcher_core/                         plain C++17, no ESP-IDF includes: unit-tested on the PC
    include/dispatcher/  src/
      bthome triggers learn config models eventlog    decoder, trigger engine, learn flow, config document
      auth switchbot switchbot_commands               password/sessions, SwitchBot client, command table
      app api                                         the state, the press flow, the JSON API
      platform.hpp                                    the interface to the outside (time, crypto, storage, HTTP, worker)
  main/                                               ESP-IDF layer: glue that implements Platform
    main wifi_manager mdns_service usb_msc config_manager status_led dual_logger   (from wol-bridge)
    esp_platform ble_scanner web_server
  test/                                               host tests: `test/run.sh`
  tools/                                              host_server (the real core on the PC), gen_switchbot_commands.py
```

The API, authentication, SwitchBot and press-flow logic only talk to the `Platform` interface, so they
run unchanged in the host tests (with a fake platform) and on the device.

### Concurrency

One mutex guards the application state (engine, learn session, config, log). NimBLE's callback only
filters and queues advertisements; a worker task processes them. Actions (HTTP, SwitchBot) run on their
own task, one at a time, because a TLS request can take seconds. Web requests, including the ones that
call out to the network ("Run now", SwitchBot lists), are answered on `esp_http_server`'s own thread, so
the page waits for them and other page requests queue behind a slow one. (An asynchronous variant was
tried first; on the board a Save request was left unserved for up to a minute after one.)

### Flash and memory

8 MB flash: a 3 MB app partition (NimBLE, TLS and the web assets will take about 2 MB), the 1 MB
`storage` FAT drive with `config.toml` (the drag-and-drop drive), and a separate 1 MB `state` NVS
partition for `config.json` and the password hash, so credentials are not on the USB-visible drive and
are not overwritten when the firmware is flashed. NVS rather than FAT because a FAT save on wear
levelling took about 3 s (dozens of 512-byte writes, each rewriting a 4 KB flash block); an NVS save
takes milliseconds and is atomic. NimBLE is built as an observer only, to save RAM.

### Bluetooth

Scanning is continuous and passive, with duplicate filtering turned off in the controller (it would
hide the new packet ids from a device that is already known). The callback does the cheapest possible
check, "is this a 0xFCD2 service-data block?", and drops everything else. Wi-Fi and BLE share one
radio, so scan timing is something to tune by measurement (a press is repeated 15-30 times over about a
second, which helps).

## Configuration (`config.toml`)

Same file and keys as `wol-bridge`, plus: `hostname`, `http_port`, `led_pin`, `led_active_low`,
`buzzer_pin`, `buzzer_active_low`, `wifi_ssid[0-9]` / `wifi_password[0-9]`, and the tuning keys `ble_scan_interval_ms`,
`ble_scan_window_ms`, `ble_scan_active` and `pbkdf2_iterations` (default 20000; the time one login
takes is logged at boot, raise it if there is room).

**Build-time secrets filter.** If `ESP32_CONFIG_TOML` points at a shared override file, only the keys
above are copied into the drive image. The shared file also holds other projects' tokens, which must
never end up on the USB-visible drive.

## Testing

- `test/run.sh` builds and runs the unit tests of `dispatcher_core` on the PC (g++, no exceptions or
  RTTI, AddressSanitizer and UndefinedBehaviorSanitizer; `-n` skips the sanitizers). Check its exit
  status, not just the output.
- `tools/run_host_server.sh` runs the real core and API on the PC with the shared web page, so the
  page can be exercised in a browser or by script without a board.
- Config validation was compared against the Python implementation on a corpus of valid and invalid
  documents; the two agree except for the deviations below.

## Deviations from the Python version

Harmless differences found by that comparison: integral floats (`2.0`) are accepted where an integer is
expected; JSON `true` is not accepted as a version; ids ending in a newline are rejected; mixed MAC
separators are rejected; requests to SwitchBot use compact JSON. Password hashes are not portable and
sessions end at a reboot (see the table above).

## Flashing

`./01-install.sh` builds and flashes the first `/dev/ttyACM*` it finds. Check what that board currently
runs before using it. Run `00-build.sh` to build only.

## Untested on hardware

NimBLE scanning together with Wi-Fi, heap headroom, how long PBKDF2 takes, `esp_http_client`
timeouts and error mapping, wildcard routes and async handlers in `esp_http_server`, the USB drive,
SNTP, mDNS, and the atomic save to the `state` partition.

## Not in this version

Zigbee (the S3 has no 802.15.4 radio), HTTPS for the web page, over-the-air updates, encrypted BTHome
payloads, a Wi-Fi setup access point.

## Open items

- The ESP32-S3-DevKitC-1 on the bench currently shows up as `/dev/ttyACM0`; do not flash it without
  checking that it isn't running another project's firmware.
- Measure BLE scanning reliability while Wi-Fi is busy, once the firmware runs.
