# button-dispatcher (ESP32-S3)

A standalone ESP32-S3 version of the BLE button dispatcher: it turns BLE (BTHome) button presses,
such as those of Shelly BLU remotes, into HTTP/HTTPS requests and SwitchBot commands or scenes. Set
it up once from a web page; no Raspberry Pi needed. A second board running the same firmware can act as a
[satellite](#satellites) that extends the Bluetooth range.

**Status: the firmware builds and the core is tested on the PC; it has not been flashed or run on a board yet.**

This is a port of [`pi/button-dispatcher`](../../pi/button-dispatcher/README.md) (Python, Raspberry
Pi, installed and working). That README is the reference for **behavior**: the press flow, triggers
and bindings, actions, web page, security model, and the config file format. This document covers
only what is different on the ESP32-S3, so the two don't drift apart.

Hardware findings behind both (packet formats, wheel behavior):
`~/history/reports/electronics/shelly-remote-switches.md`.

## One contract, two implementations

To keep the Python and C++ versions equivalent:

- **Forked web page.** `web/` started as a copy of `pi/button-dispatcher/button_dispatcher/static/` and adds
  what only this version has (the board's role and host name, satellites). Fixes to the shared parts should be
  made in both copies.
- **Same JSON API and config format.** A config exported from one imports into the other
  (credentials stay out of exports by default, as on the Pi). The routes this version adds (`/api/device`,
  `/api/satellites`, `/api/satellite/report`) keep their data out of `config.json`, so exports stay compatible.
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
  web/                                                the web page (forked from the Pi version, see above)
  components/dispatcher_core/                         plain C++17, no ESP-IDF includes: unit-tested on the PC
    include/dispatcher/  src/
      bthome triggers learn config models eventlog    decoder, trigger engine, learn flow, config document
      device satellite                                role, host name, pairing; satellite reports and forwarding
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

## Configuration (`config.toml`, `config-override.toml`)

Same file and keys as `wol-bridge`, plus: `hostname` and `hostname_append_mac` (only defaults, see
[Host name](#host-name)), `http_port`, `led_pin`, `led_active_low`,
`buzzer_pin`, `buzzer_active_low`, `wifi_ssid[0-9]` / `wifi_password[0-9]`, and the tuning keys `ble_scan_interval_ms`,
`ble_scan_window_ms`, `ble_scan_active` and `pbkdf2_iterations` (default 20000; the time one login
takes is logged at boot, raise it if there is room).

How the two files work (the override, the shared `ESP32_CONFIG_TOML`, and how secrets are moved out of the
files at boot) is described in the top-level
[README](../../README.md#configuration-files-configtoml-and-config-overridetoml), so all ESP32 projects share
one description. This project's implementation: `main/config_manager.cpp` reads the files and talks to NVS (default
partition, namespace `secrets`, one blob), `components/dispatcher_core/src/config_secrets.cpp` holds the
parsing and scrubbing (covered by `test/test_config_secrets.cpp`). SwitchBot credentials and the password hash are
not in the config files at all; they live in the `state` partition and are set on the web page.

## Host name

The board's network name is `<base>-<Wi-Fi MAC>`, for example `dispatcher-24-0a-c4-12-34-56.local`, so that
boards flashed with the same `config.toml` (the shared `ESP32_CONFIG_TOML` override) don't claim the same
name. Settings, This board, changes the base and turns the MAC suffix off. The change takes effect at once for
mDNS (`.local`); the name sent to the router with DHCP changes at the next lease renewal. The base defaults to
`config.toml`'s `hostname` (`dispatcher` if unset), and the suffix to `hostname_append_mac` (`true` if unset);
what is set on the page is stored in `device.json` and wins. To give one board an exact name from the first
boot, build it with `ESP32_CONFIG_TOML` pointing at a copy of the shared override with
`hostname_append_mac = false` added (or add it to `config-override.toml` on the board's USB drive later); with
the default `hostname`, it comes up as `dispatcher.local`.

## Satellites

The Bluetooth range of one board limits where its buttons work. A satellite is a second board with this
firmware, set to the satellite role and placed near the far buttons: it scans the same way, and forwards each
new press over Wi-Fi to its dispatcher, which handles it exactly like a press it heard itself (same dedupe,
bindings, cooldowns and "Find device"). The dispatcher logs these presses with "(via <satellite name>)".

```
button --BLE--> satellite: scan -> press/turn? -> drop repeats -> signed POST --+
                                                                               |
dispatcher: dedupe -> bindings -> actions  <---- /api/satellite/report <-------+
            (its own scanner feeds the same path)
```

### Setting one up

1. Flash the firmware on the second board (same `config.toml` / override for Wi-Fi).
2. On the dispatcher's page: Satellites, Add satellite, give it a name. A pairing code is shown once.
3. On the satellite's page (its name is on its serial log, or find it by MAC): set a password, then Settings,
   This board, Role: Satellite. On its Status tab, paste the pairing code and Pair.
4. The Status tab says "Reporting to ..." and the dispatcher lists the satellite as Online.

The pairing code is `bdsat1:<id>:<secret>@<dispatcher host>:<port>`; its address part can be edited (for
example to an IP address) before pasting. Removing a satellite on the dispatcher revokes its secret.

On a satellite, the Devices, Bindings and Satellites tabs are hidden (its buttons and bindings are kept, and
come back if it is made a dispatcher again), and the buzzer stays silent because nothing fires there. The LED
blinks slowly (1 s on, 1 s off) while the dispatcher cannot be reached.

### Protocol and security

One request, sent for presses and every 30 s as a heartbeat:

```
POST /api/satellite/report
X-Satellite: <satellite id>
X-Signature: <hex HMAC-SHA256 of the body with the satellite's 32-byte secret>
{"time": <Unix ms>, "boot": "<random per boot>", "seq": <n>, "uptime_s": .., "dropped": .., "queued": ..,
 "events": [{"mac": "AA:BB:CC:DD:EE:FF", "rssi": -52, "data": "<BTHome payload hex>", "age_ms": 40}]}
```

- Each satellite has its own secret, created by the dispatcher and stored only in the two boards' `state`
  partitions (never in `config.json`, exports or the USB drive). The report route needs no session or CSRF
  header; the signature is its authentication.
- Replays: the dispatcher refuses a report whose `time` is more than 30 s from its own clock, and requires `seq`
  to grow within one `boot` (a new `boot` must carry a newer time than anything accepted). So both boards need
  the clock from NTP; a satellite holds its reports until its clock is set. After a dispatcher restart, a report
  recorded in the last 30 s could be replayed once, but its events are then either more than 5 s old (dropped,
  see below) or still within the dedupe window, so nothing fires twice.
- A board in the satellite role refuses reports, so satellites cannot be chained.
- The requests are plain HTTP: the signature protects integrity, and the contents (button presses) are not
  secret.
- Stale presses: events more than 5 s old (time at the satellite plus time on the way) are dropped, so a
  "wake the PC" never fires late. A satellite retries a failed report every second until its events are 5 s
  old, and queues at most 32 events.
- Several receivers hearing the same press (the dispatcher and satellites) are fine: the dedupe remembers
  several recent packet ids per device. Data flows one way and nothing re-forwards what it received, so there
  are no loops.

## Testing

- `test/run.sh` builds and runs the unit tests of `dispatcher_core` on the PC (g++, no exceptions or
  RTTI, AddressSanitizer and UndefinedBehaviorSanitizer; `-n` skips the sanitizers). Check its exit
  status, not just the output.
- `tools/run_host_server.sh` runs the real core and API on the PC with the web page, so the page can be
  exercised in a browser or by script without a board. Two of them with different `--port`, `--data-dir` and
  `--mac` can be paired as dispatcher and satellite (edit the pairing code's address to `127.0.0.1:<port>`);
  `GET /test/packet` on the satellite then forwards to the dispatcher.
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

Satellite mode as a whole (forwarding from one board to another, `.local` name resolution by
`esp_http_client`, the forwarding task's stack size, live mDNS renames), NimBLE scanning together with Wi-Fi, heap headroom, how long PBKDF2 takes, `esp_http_client`
timeouts and error mapping, wildcard routes and async handlers in `esp_http_server`, the USB drive,
SNTP, mDNS, and the atomic save to the `state` partition.

## Not in this version

Zigbee (the S3 has no 802.15.4 radio), HTTPS for the web page, over-the-air updates, encrypted BTHome
payloads, a Wi-Fi setup access point.

## Open items

- The ESP32-S3-DevKitC-1 on the bench currently shows up as `/dev/ttyACM0`; do not flash it without
  checking that it isn't running another project's firmware.
- Measure BLE scanning reliability while Wi-Fi is busy, once the firmware runs.
