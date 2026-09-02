# nsbackend-esp32s3

A high-performance C++ port of **nsbackend-pico** for the **ESP32-S3** (targeting the **Seeed Studio XIAO ESP32-S3**), built on the **ESP-IDF** framework (`v5.5.5` LTS).

---

## 1. Overview

`nsbackend-esp32s3` allows an ESP32-S3 microcontroller to function as a Nintendo Switch controller backend:
1. **USB HID Gamepad**: Emulates a HORI Pokken Nintendo Switch controller over native USB OTG (`VID: 0x0f0d`, `PID: 0x0092`), transmitting 8-byte HID reports to the Nintendo Switch.
2. **USB CDC ACM Serial Console**: Provides `/dev/ttyACM0` for real-time serial logging and debugging (`idf.py monitor` / `tio`).
3. **USB MSC Flash Storage**: Exposes the internal Wear Levelling FATFS partition as a USB flash drive for easy drag-and-drop editing of `config.toml`.
4. **Wi-Fi Multi-AP Manager**: Connects to configured Wi-Fi access points with automatic scan-based fallback and background reconnection.
5. **mDNS Service**: Advertises the device hostname as `nscon.local` with service `_nscon._tcp` on port `10100`.
6. **TCP Command Server**: Listens on port `10100` for streaming controller commands from frontend clients (`nsfrontend` or interactive pipelines), parsing directional sticks, button presses, and auto-release timers.
7. **Physical GPIO Buttons**: Supports hardware buttons with internal pull-ups, debouncing, and opposing D-pad direction cancellation.
8. **Status LED**: Indicates system lifecycle states via onboard user LED patterns.
9. **FATFS Configuration**: Loads Wi-Fi credentials and runtime settings from `config.toml` and optional `config-override.toml` on the FATFS partition.

---

## 2. Hardware Pinout (Seeed Studio XIAO ESP32-S3)

| Pin Name | ESP32-S3 GPIO | Function | Description |
| :--- | :--- | :--- | :--- |
| **D0** | `GPIO1` | Button A | Active-low, internal pull-up |
| **D1** | `GPIO2` | D-pad DOWN | Active-low, internal pull-up |
| **D2** | `GPIO3` | D-pad LEFT | Active-low, internal pull-up |
| **D3** | `GPIO4` | D-pad RIGHT | Active-low, internal pull-up |
| **D4** | `GPIO5` | D-pad UP | Active-low, internal pull-up |
| **D5** | `GPIO6` | Button B | Active-low, internal pull-up |
| **D10** | `GPIO9` | Buttons L + R | Simultaneous L and R buttons |
| **LED** | `GPIO21` | Status LED | Active-low yellow onboard user LED |
| **USB D-** | `GPIO19` | USB OTG D- | Native USB OTG data negative |
| **USB D+** | `GPIO20` | USB OTG D+ | Native USB OTG data positive |

---

## 3. Progress LED Patterns

| State | Lifecycle Phase | LED Pattern |
| :--- | :--- | :--- |
| **Boot / Hardware Init** | Power-on, flash mount, TinyUSB init | **Solid ON** |
| **Wi-Fi Connecting** | Scanning and associating with Wi-Fi AP | **0.1s ON, 1.0s OFF** (1 short blink) |
| **Setting up TCP** | Wi-Fi connected, starting mDNS & TCP server | **0.1s ON, 0.1s OFF, 0.1s ON, 1.0s OFF** (2 short blinks) |
| **Waiting for Client** | TCP server listening on port 10100 | **0.5s ON, 0.5s OFF** (Slow blink) |
| **Client Connected** | Client connected & streaming commands | **1.0s ON, 1.0s OFF** (Heartbeat) |
| **Wi-Fi Error / Reconnecting** | Wi-Fi unconfigured / failed / reconnecting | **0.1s ON, 0.1s OFF** (Rapid strobe) |

---

## 4. Architecture

```
                                +---------------------------+
                                |      Nintendo Switch      |
                                +-------------^-------------+
                                              | (USB HID OTG)
+-------------------+           +-------------v-------------+
|   TCP Client      |  (Wi-Fi)  |    nsbackend-esp32s3      |
| (nsfrontend / CLI)| --------> |  - TCP Server (Port 10100)|
+-------------------+           |  - Controller State Engine|
                                |  - Composite TinyUSB:     |
+-------------------+           |    * HID Gamepad          |
|  Physical Buttons | --------> |    * CDC Serial Console   |
|   (D0-D5, D10)    |           |    * MSC Storage (FATFS)  |
+-------------------+           |  - Wi-Fi Multi-AP & mDNS  |
                                |  - GPIO Button Manager    |
                                |  - FATFS Config Reader    |
                                +---------------------------+
```

### Module Responsibilities
- **`main/main.cpp`**: Application lifecycle orchestrator, initializes storage, Wi-Fi, USB, GPIO, and TCP services.
- **`main/config_manager.hpp/.cpp`**: Mounts Wear Levelling FATFS filesystem and parses `config.toml` / `config-override.toml`.
- **`main/status_led.hpp/.cpp`**: Manages LED state machine (`INITIALIZING`, `WAITING_CLIENT`, `CLIENT_CONNECTED`).
- **`main/gamepad_hid.hpp/.cpp`**: Initializes TinyUSB composite stack (HORI Pokken Gamepad HID + CDC Serial + MSC Flash Storage).
- **`main/controller_state.hpp/.cpp`**: Maintains button bitmasks, analog stick coordinates, auto-release timers, and merges GPIO button state.
- **`main/gpio_buttons.hpp/.cpp`**: Monitors physical GPIO pins with 15ms software debouncing and applies inputs to `ControllerState`.
- **`main/wifi_manager.hpp/.cpp`**: Implements multi-AP candidate selection, Wi-Fi scanning, and reconnection logic.
- **`main/mdns_service.hpp/.cpp`**: Initializes ESP-IDF mDNS responder for `nscon.local`.
- **`main/tcp_server.hpp/.cpp`**: Socket server running FreeRTOS task to accept client connections and parse streaming controller commands.

---

## 5. Implementation Status

- [x] **Phase 1: Project Skeleton & Build Setup**
  - CMake build system, custom `partitions.csv` (8MB Flash with SPIFFS), `sdkconfig.defaults` (TinyUSB, FreeRTOS 1kHz tick rate, LwIP).
- [x] **Phase 2: Configuration & Storage Layer**
  - Implemented `ConfigManager` reading `config.toml` and optional `config-override.toml` from SPIFFS partition.
- [x] **Phase 3: Status LED & GPIO Hardware Buttons**
  - Implemented `StatusLed` FreeRTOS task with 3-state blinking patterns.
  - Implemented `GpioButtonManager` with 15ms debouncing and active-low input handling for D0–D5, D10.
- [x] **Phase 4: TinyUSB HID Gamepad Driver**
  - Configured TinyUSB HORI Pokken Controller (`VID: 0x0f0d`, `PID: 0x0092`) 8-byte report descriptor and atomic report dispatching.
- [x] **Phase 5: Wi-Fi Multi-AP Manager & mDNS**
  - Implemented Wi-Fi station mode with multi-AP fallback, auto-scan, background reconnection, and mDNS responder (`nscon.local` / `_nscon._tcp`).
- [x] **Phase 6: Controller State Engine & TCP Command Server**
  - Implemented `ControllerState` handling all button/stick tokens, duration commands, auto-release queues, and thread-safe locking.
  - Implemented `TcpServer` socket server streaming inputs at port `10100`.
- [x] **Phase 7: Compilation, Verification & Helper Scripts**
  - Created `00-build.sh`, `01-install.sh`, and `02-monitor.sh`.
  - Verified clean build with ESP-IDF `v5.5.5` toolchain targeting `esp32s3`.

---

## 6. Development Environment Setup

To set up the ESP-IDF build environment on Ubuntu / Debian:

### 1. Install System Dependencies (apt)
```bash
sudo apt-get update
sudo apt-get install -y git wget flex bison gperf python3 python3-pip python3-venv cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0
```

### 2. Clone ESP-IDF (v5.5.5 LTS)
Clone the repository to `~/esp-idf` with submodules:
```bash
git clone -b v5.5.5 --recursive https://github.com/espressif/esp-idf.git ~/esp-idf
```

### 3. Install ESP32-S3 Toolchain & Dependencies
Run the ESP-IDF installer targeting the `esp32s3` chip:

> [!NOTE]
> Prepending `/usr/bin` to `PATH` ensures the system Python is used. ESP-IDF creates and manages its own isolated virtual environment under `~/.espressif/python_env/` and will fail (`ERROR: This script was called from a virtual environment`) if invoked from within an existing virtual environment or if a user virtualenv precedes `/usr/bin` in `$PATH`.

```bash
cd ~/esp-idf
PATH="/usr/bin:$PATH" ./install.sh esp32s3
```

### 4. Environment Activation
[`./00-build.sh`](file:///home/omakoto/cbin/src/raspberry-pi-pico/nsbackend-esp32s3/00-build.sh) and [`./01-install.sh`](file:///home/omakoto/cbin/src/raspberry-pi-pico/nsbackend-esp32s3/01-install.sh) automatically detect and source `${IDF_PATH}/export.sh` or `~/esp-idf/export.sh`.

If you prefer to run `idf.py` manually in your shell:
```bash
. ~/esp-idf/export.sh
```

### 5. Serial Port Permissions (Optional)
Add your user to the `dialout` group to allow flashing over USB without root privileges:
```bash
sudo usermod -a -G dialout $USER
```
*(Log out and log back in for group changes to take effect)*

---

## 7. Building & Flashing

### 1. Configure Wi-Fi in `fatfs_data/config.toml`
Edit `fatfs_data/config.toml` (or create `fatfs_data/config-override.toml`) before building:
```toml
hostname = "nscon"
tcp_port = 10100
wifi_ssid = "YOUR_WIFI_SSID"
wifi_password = "YOUR_WIFI_PASSWORD"
```
*(Alternatively, you can edit `config.toml` directly on the mounted USB flash drive on your PC after flashing.)*

### 2. Build the Firmware
Run [`./00-build.sh`](file:///home/omakoto/cbin/src/raspberry-pi-pico/nsbackend-esp32s3/00-build.sh):
```bash
cd ~/cbin/src/raspberry-pi-pico/nsbackend-esp32s3
./00-build.sh
```

### 3. Flash to ESP32-S3
Put the board into Bootloader mode (Hold **B**, press & release **R**, release **B**) and run [`./01-install.sh`](file:///home/omakoto/cbin/src/raspberry-pi-pico/nsbackend-esp32s3/01-install.sh):
```bash
./01-install.sh
# or specify the port explicitly:
./01-install.sh /dev/ttyACM0
```

---

## 8. Serial Console Monitoring

Because Composite USB is enabled, the ESP32-S3 exposes `/dev/ttyACM0` for console output alongside the USB HID gamepad.

Run [`./02-monitor.sh`](file:///home/omakoto/cbin/src/raspberry-pi-pico/nsbackend-esp32s3/02-monitor.sh):
```bash
./02-monitor.sh
# or specify the port explicitly:
./02-monitor.sh /dev/ttyACM0
# Exit with Ctrl+]
```

Or use `tio` / `picocom`:
```bash
tio /dev/ttyACM0
# Exit with Ctrl+t then q
```

---

## 9. Running `nsfrontend` & Sending Commands

Once the ESP32-S3 is connected to Wi-Fi, you can stream controller inputs from your PC to the device (`nscon.local:10100`):

### 1. Streaming from a Physical Controller
```bash
# Using nsfrontend with a physical controller (via Bash /dev/tcp):
nsfrontend -j /dev/input/js0 -o >(cat > /dev/tcp/nscon.local/10100)

# Or piping directly via nc (netcat):
nsfrontend -j /dev/input/js0 | nc nscon.local 10100
```

### 2. Sending One-Off Commands Directly Over TCP
```bash
# Press and auto-release button A:
echo "a" | nc nscon.local 10100

# Press and auto-release Home button:
echo "home" | nc nscon.local 10100
```
