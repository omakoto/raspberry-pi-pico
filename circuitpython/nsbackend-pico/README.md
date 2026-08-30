# nsbackend-pico

Nintendo Switch USB Controller backend running over TCP / mDNS for Raspberry Pi Pico 2 W, ESP32, and compatible CircuitPython boards.

This project implements a CircuitPython backend for [`raspberry-switch-control`](https://github.com/omakoto/raspberry-switch-control), allowing commands from `nsfrontend` or scripts to control a Nintendo Switch over Wi-Fi/TCP.

## Features

- **USB Composite Controller Emulation**: Exposes a HORI Pokken Controller USB HID Gamepad descriptor alongside standard USB CDC serial console and Mass Storage.
- **TCP Command Server**: Listens on port `10100` (configurable in `config.toml`) and advertises the board hostname via mDNS (default `nscon.local`).
- **Compatible with `nsfrontend`**: Accepts the exact streaming command protocol sent by `nsfrontend`, including digital buttons, D-pad, stick axes, and auto-release single-token commands.
- **Auto Reconnect & Safe Disconnect**: Detects client disconnections / connection drops and resets all inputs to neutral to prevent stuck button presses.
- **LED Progress Indicator**: Shows the 4-step connection and server lifecycle state using the onboard user LED.

## Progress LED Patterns

| State | Lifecycle Phase | LED Pattern |
| :--- | :--- | :--- |
| **Initializing** | Wi-Fi connecting & TCP server / mDNS setup | **Solid ON** |
| **Waiting for Client** | Server listening on port 10100 | **3 blinks** (0.2s ON, 0.2s OFF x 3, 0.5s pause) |
| **Client Connected** | Client connected & streaming commands | **Heartbeat** (1.0s ON, 1.0s OFF) |

## Usage

1. Configure your Wi-Fi credentials in [`config.toml`](file:///home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/nsbackend-pico/config.toml):
   ```toml
   hostname = "nscon"
   wifi_ssid = "YOUR_WIFI_SSID"
   wifi_password = "YOUR_WIFI_PASSWORD"
   tcp_port = 10100
   log = false
   ```

2. Flash and run on your board:
   ```bash
   ./nsbackend-pico.py
   ```

3. Connect your host controller or send commands from PC:
   ```bash
   # Using nsfrontend with a physical controller:
   nsfrontend -j /dev/input/js0 -o /dev/tcp/nscon/10100

   # Or sending commands directly over TCP:
   echo "a" | nc nscon.local 10100
   ```
