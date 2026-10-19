# button-dispatcher (Raspberry Pi)

Turns BLE buttons (Shelly BLU remotes and any other BTHome button) into HTTP/HTTPS requests and
SwitchBot actions. Everything is configured from a web page on the Pi; no file editing.

**Status: built and tested on a PC** (unit tests, plus a headless-Chrome run through the whole web
UI). Not yet installed on a Raspberry Pi, and not yet tried with a real button press on the
installed service.

This lives under `pi/` as an exception to the repo's "microcontroller projects" scope: it targets a
Raspberry Pi (Python, BlueZ), but the design keeps the BTHome parser, trigger matching and config
format small and language-neutral so it can be ported to an ESP32 later.

Hardware findings behind this design (packet formats, wheel behavior, rejected alternatives):
`~/history/reports/electronics/shelly-remote-switches.md`.

## What it does

1. **Learns a button press**: on the Bindings tab you click "Add by pressing a button" and press the
   BLE button. The page shows the press it hears, asks you to press it once more to confirm, and
   opens a binding for it. A remote with several physical buttons is one *device*; every physical
   button gets its own binding, so you repeat this for each button you want to use.
2. **Binds triggers to actions**: "button 2, single press, channel 3 of the desk remote" -> "GET
   `http://wol.local/wake`".
3. **Runs actions** when the button is pressed: a plain HTTP/HTTPS request (this covers IFTTT
   webhooks and anything else), a SwitchBot device command, or a SwitchBot scene.

## Architecture

```
 BLE button --adverts--> [scanner (bleak/BlueZ)] --> [BTHome decoder] --> [dedupe + trigger matcher]
                                                                                   |
 browser <--> [web server (aiohttp)] <--> [config store] ----------------> [action runner]
                  |  login, learn mode, bindings,                              |-- HTTP/HTTPS request
                  |  settings, import/export, event log                        |-- SwitchBot command
                                                                               '-- SwitchBot scene
```

One Python process, one asyncio loop (scanner and web server share it), run by systemd. Only
dependencies: `aiohttp` and `bleak`; the web page is plain HTML and JavaScript with no build step and
no CDN, so it works with no internet on the LAN.

## Inputs: BTHome buttons

- Decodes BTHome v2 service data (UUID `0xFCD2`): packet id, battery, button events (`0x3A`),
  wheel (`0x3C`), channel (`0x60`). Unencrypted payloads only in v1; encrypted ones are reported in
  the log as "encrypted (unsupported)".
- **Dedupe** on (MAC, packet id): a sender repeats each event 15-30 times, so only the first copy is
  acted on. The id wraps at 255, so only a change from the last id counts as new.
- **Wheel** (Shelly Remote Control): a turn sends a "start" packet (0 steps) immediately and a final
  packet with the step count after it stops; reversing direction cancels the turn in progress, and
  wiggling produces bursts of start packets. The matcher fires on the **start** packet, ignores the
  step count, and applies a per-binding **cooldown** (default 700 ms).
- **Channel** (Shelly Remote Control, 4 LEDs): every packet carries the channel (0-3). Moving the
  LED sends nothing. A trigger can match one channel or any channel.
- Devices are identified by MAC address. Match is on the advertised address (the advertised name,
  e.g. `SBBT-104CUS`, is a model code shared by all units, so it is shown but never used to match).

### "Press a button" flow

Reached from "Add by pressing a button" (Bindings tab) or "Find device" (Devices tab).

1. The page says "Press the button you want to add".
2. The scanner collects BTHome packets that carry a button/wheel event for up to 30 s, preferring
   the **strongest signal** (so a neighbor's button is not picked over the one next to the Pi).
3. The page shows the first candidate (MAC, model name, signal strength, which control and event:
   e.g. "button 2, press, channel 3") and asks: "Press the same button **once more** (as a
   separate press, not a double press)".
4. If the second press has the same MAC, channel, control and event as the first (and a new packet
   id), it is confirmed. Otherwise it explains the mismatch and retries.
5. A new device is named and saved; a device that is already known is left alone and the page
   says so ("This is a button on 'Desk remote'..."). Either way the binding editor opens with the
   device, physical button, press type and channel filled in. ("Find device" can also save the
   device without creating a binding.)

## Triggers and bindings

A **device** (called `button` in the config file and API) is a named remote or button (MAC +
model). All the physical buttons of a remote share its device. A **binding** is: trigger + action +
settings.

- Trigger: `button` (which device), `channel` (0-3 or any), `input` (`button:1`..`button:4`, or
  `wheel:left` / `wheel:right`; these are BTHome's rotate-left/right events), `event` (`press`,
  `double_press`, `triple_press`, `long_press`, `long_double_press`, `long_triple_press`,
  `hold_press`; `turn` for the wheel). The Shelly remotes send press, double press, triple press
  and hold; the long-press codes are accepted for other BTHome devices.
- Several bindings may share one trigger; all of them run.
- Settings: name, enabled, cooldown (ms).

## Actions

| Type | Fields | Notes |
|---|---|---|
| `http` | method (GET default, POST), URL, optional headers, optional body, timeout | HTTP and HTTPS (certificates verified). Redirects followed. No automatic retries, so a press never fires twice. |
| `switchbot_command` | device, command, parameter, command type | Pick the device from the list fetched from the SwitchBot API; the command list comes from a built-in table (below). |
| `switchbot_scene` | scene | Pick from the scene list fetched from the API. |

Every binding has a **Run now** button, and every run (success or error, HTTP status, SwitchBot
`statusCode`) goes into the event log.

### SwitchBot

- Credentials (token and secret) are typed in the Settings page and stored on the Pi. The fields
  are write-only: the page never shows them back.
- Signing and requests follow SwitchBot Open API v1.1 (HMAC-SHA256 of token + timestamp + nonce).
  The same logic is already in `~/cbin/misc/switchbot` (a CLI); this project carries its own copy
  of the client and the command table (the Pi can't depend on `~/cbin`).
- The device and scene lists are fetched on demand with a "Refresh" button.
- The command table (command name, parameter hint, description per device type) is copied from
  that CLI's generated table, with the junk entries removed (see "Open items").

## Web UI

One page with four sections: **Devices** (list, rename, delete, Find device), **Bindings** (list,
add by pressing a button or manually, edit, enable, Run now), **Settings** (password, SwitchBot
credentials, import/export), and
**Log** (recent BLE events, matched bindings and action results, refreshed every second).
A "Simulate press" control fires a trigger without the physical button.

## Security

- **Password set on first visit**; everything (page and API) requires login. Passwords are stored as
  salted hashes (stdlib `hashlib.scrypt`). Login attempts are rate-limited.
- Sessions use a random cookie (`HttpOnly`, `SameSite=Strict`); state-changing requests also need a
  custom header, which blocks cross-site form posts.
- **Plain HTTP only** in v1 (LAN use). The password and SwitchBot token cross the LAN
  unencrypted; do not expose the Pi to the internet.
- SwitchBot token/secret and the password hash are stored in the data directory with mode `0600`.
- Forgot the password: `sudo button-dispatcher --reset-password` (or delete `auth.json`).

## Config and import/export

Data directory `/var/lib/button-dispatcher/` (service user only):

- `config.json`: buttons, bindings, settings, SwitchBot credentials.
- `auth.json`: password hash and cookie-signing key. Never exported.

```json
{
  "version": 1,
  "switchbot": {"token": "...", "secret": "..."},
  "buttons": [{"id": "desk", "name": "Desk remote", "mac": "AA:BB:CC:DD:EE:FF", "model": "SBBT-104CUS"}],
  "bindings": [{
    "id": "wake-pc", "name": "Wake PC", "enabled": true, "cooldown_ms": 700,
    "trigger": {"button": "desk", "channel": null, "input": "button:2", "event": "press"},
    "action": {"type": "http", "method": "GET", "url": "http://wol.local/wake"}
  }]
}
```

- **Export** downloads this JSON **without** SwitchBot credentials by default; an explicit
  "include credentials" checkbox adds them. The password hash and cookie key are never included.
- **Import** takes the same JSON, validates it, and either merges or replaces. Missing credentials
  in the file leave the stored ones untouched.

## Install

From the host PC (the Pi must already be on the network with SSH enabled):

```
./deploy.sh pi9.local            # or user@pi9.local
```

`deploy.sh` copies the project over SSH and runs `install.sh` on the Pi, which:

1. installs the few apt packages needed (venv support; BlueZ is normally already there),
2. creates a `button-dispatcher` service user in the `bluetooth` group,
3. creates a venv under `/opt/button-dispatcher/` and installs the pinned requirements,
4. installs and starts the systemd unit (web UI on `http://<host>/`, port 80 via
   `CAP_NET_BIND_SERVICE`; the port can be changed),
5. prints the URL.

Running it again upgrades in place and keeps `/var/lib/button-dispatcher/`. `uninstall.sh` removes
the service (and, only if asked, the data).

## Project layout

```
pi/button-dispatcher/
  README.md  deploy.sh  install.sh  uninstall.sh  requirements.txt  button-dispatcher.service
  button_dispatcher/
    __main__.py   app.py        command line; web server, JSON API and application state
    scanner.py                  BLE scanning (bleak), restarts itself after errors or stalls
    bthome.py                   BTHome decoding
    triggers.py  models.py      dedupe, matching, cooldown; data types
    learn.py                    "Press a button" state machine
    actions.py  switchbot.py  switchbot_commands.py
    config.py   auth.py   eventlog.py
    static/     index.html, app.js, style.css, favicon.svg
    *_test.py                   unit tests next to the modules
```

## Development and testing

- The same code runs on the Linux PC (it has BlueZ and a Bluetooth adapter), so it can be tried
  against the real remotes first: `python3 -m button_dispatcher --data-dir /tmp/bd --port 8080`
  (add `--no-ble` to skip Bluetooth). It needs `aiohttp` and `bleak`; in a virtualenv:
  `python3 -m venv .venv && .venv/bin/pip install -r requirements.txt`.
- Run the unit tests from the project directory:
  `python3 -m unittest discover -p '*_test.py'` (use the virtualenv's python; without `aiohttp`
  the modules that need it are skipped). They need no Bluetooth: the decoder and matcher use the
  real packets captured from the Shelly remotes, and config, auth, import/export, the learn state
  machine, the actions and the whole web API (`app_test.py`) run against fake local HTTP and
  SwitchBot servers.
- The web page was additionally driven end to end in headless Chrome (sign-in, adding presses with
  injected adverts, bindings, Run now, SwitchBot pickers, log, settings, import/export, phone width,
  dark mode); that harness is not part of the repository.

## Status of the build

1. Done: decoder, trigger engine, config, auth, learn flow, actions, SwitchBot client, web server,
   web page, installer scripts.
2. To do: install on `pi9.local` and try it with a real button press there; update `PROJECTS.md`.

## Not in v1

Zigbee input, HTTPS for the web UI, multiple users, encrypted BTHome payloads, action retries,
scheduled actions.

## Open items

- `~/cbin/misc/switchbot`'s command table has non-device entries from the API doc parser
  (`endTime`, `id`, `Key Name`, `name`, `password`, `startTime`, `type`), duplicated commands under
  `Humidifier2`, and curly quotes in `Relay Switch 2PM`'s parameter hints. The copy in this project
  is cleaned; the original is untouched.
- Wheel direction naming: the first direction the user called "up" produced BTHome rotate-left
  events; the UI uses "left"/"right".
- BLE range: the Pi's own antenna decides how far from the Pi the buttons can be.
