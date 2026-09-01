# nsbackend-esp32s3

A high-performance C++ port of **nsbackend-pico** for the **ESP32-S3** (targeting the **Seeed Studio XIAO ESP32-S3**), built on the **ESP-IDF** framework (`v5.5.5` LTS).

---

## 1. Overview

`nsbackend-esp32s3` allows an ESP32-S3 microcontroller to function as a Nintendo Switch controller backend:
1. **USB HID Gamepad**: Emulates a HORI Pokken Nintendo Switch controller over native USB OTG (`VID: 0x0f0d`, `PID: 0x0092`), transmitting 8-byte HID reports to the Nintendo Switch.
2. **Wi-Fi Multi-AP Manager**: Connects to configured Wi-Fi access points with automatic scan-based fallback and background reconnection.
3. **mDNS Service**: Advertises the device hostname as `nscon.local` with service `_nscon._tcp` on port `10100`.
4. **TCP Command Server**: Listens on port `10100` for streaming controller commands from frontend clients (`nsfrontend` or interactive pipelines), parsing directional sticks, button presses, and auto-release timers.
5. **Physical GPIO Buttons**: Supports hardware buttons with internal pull-ups, debouncing, and opposing D-pad direction cancellation.
6. **Status LED**: Indicates system lifecycle states via onboard user LED patterns.
7. **SPIFFS Configuration**: Loads Wi-Fi credentials and runtime settings from `config.toml` and optional `config-override.toml`.

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
| **D6** | `GPIO43` | Buttons L + R | Simultaneous L and R buttons |
| **LED** | `GPIO21` | Status LED | Active-low yellow onboard user LED |
| **USB D-** | `GPIO19` | USB OTG D- | Native USB OTG data negative |
| **USB D+** | `GPIO20` | USB OTG D+ | Native USB OTG data positive |

---

## 3. Architecture

```
                                +---------------------------+
                                |      Nintendo Switch      |
                                +-------------^-------------+
                                              | (USB HID OTG)
+-------------------+           +-------------v-------------+
|   TCP Client      |  (Wi-Fi)  |    nsbackend-esp32s3      |
| (nsfrontend / CLI)| --------> |  - TCP Server (Port 10100)|
+-------------------+           |  - Controller State Engine|
                                |  - TinyUSB HID Gamepad    |
+-------------------+           |  - Wi-Fi Multi-AP & mDNS  |
|  Physical Buttons | --------> |  - GPIO Button Manager    |
|   (D0 - D6)       |           |  - SPIFFS Config Reader   |
+-------------------+           +---------------------------+
```

### Module Responsibilities
- **`main/main.cpp`**: Application lifecycle orchestrator, initializes storage, Wi-Fi, USB, GPIO, and TCP services.
- **`main/config_manager.hpp/.cpp`**: Mounts SPIFFS filesystem and parses `config.toml` / `config-override.toml`.
- **`main/status_led.hpp/.cpp`**: Manages LED state machine (`INITIALIZING`, `WAITING_CLIENT`, `CLIENT_CONNECTED`).
- **`main/gamepad_hid.hpp/.cpp`**: Initializes TinyUSB device stack and handles 8-byte HORI Pokken HID report formatting.
- **`main/controller_state.hpp/.cpp`**: Maintains button bitmasks, analog stick coordinates, auto-release timers, and merges GPIO button state.
- **`main/gpio_buttons.hpp/.cpp`**: Monitors physical GPIO pins with 15ms software debouncing and applies inputs to `ControllerState`.
- **`main/wifi_manager.hpp/.cpp`**: Implements multi-AP candidate selection, Wi-Fi scanning, and reconnection logic.
- **`main/mdns_service.hpp/.cpp`**: Initializes ESP-IDF mDNS responder for `nscon.local`.
- **`main/tcp_server.hpp/.cpp`**: Socket server running FreeRTOS task to accept client connections and parse streaming controller commands.

---

## 4. Implementation Status

- [x] **Phase 1: Project Skeleton & Build Setup**
  - CMake build system, custom `partitions.csv` (8MB Flash with SPIFFS), `sdkconfig.defaults` (TinyUSB, FreeRTOS 1kHz tick rate, LwIP).
- [x] **Phase 2: Configuration & Storage Layer**
  - Implemented `ConfigManager` reading `config.toml` and optional `config-override.toml` from SPIFFS partition.
- [x] **Phase 3: Status LED & GPIO Hardware Buttons**
  - Implemented `StatusLed` FreeRTOS task with 3-state blinking patterns.
  - Implemented `GpioButtonManager` with 15ms debouncing and active-low input handling for D0–D6.
- [x] **Phase 4: TinyUSB HID Gamepad Driver**
  - Configured TinyUSB HORI Pokken Controller (`VID: 0x0f0d`, `PID: 0x0092`) 8-byte report descriptor and atomic report dispatching.
- [x] **Phase 5: Wi-Fi Multi-AP Manager & mDNS**
  - Implemented Wi-Fi station mode with multi-AP fallback, auto-scan, background reconnection, and mDNS responder (`nscon.local` / `_nscon._tcp`).
- [x] **Phase 6: Controller State Engine & TCP Command Server**
  - Implemented `ControllerState` handling all button/stick tokens, duration commands, auto-release queues, and thread-safe locking.
  - Implemented `TcpServer` socket server streaming inputs at port `10100`.
- [x] **Phase 7: Compilation, Verification & Helper Scripts**
  - Created `00-build.sh` and `01-install.sh`.
  - Verified clean build with ESP-IDF `v5.5.5` toolchain targeting `esp32s3`.

---

## 5. Building & Flashing

### 1. Configure Wi-Fi in `spiffs_data/config.toml`
Edit `spiffs_data/config.toml` (or create `spiffs_data/config-override.toml`) to specify your Wi-Fi credentials:
```toml
hostname = "nscon"
tcp_port = 10100
wifi_ssid = "YOUR_WIFI_SSID"
wifi_password = "YOUR_WIFI_PASSWORD"
```

### 2. Build the Firmware
Run [`./00-build.sh`](file:///home/omakoto/cbin/src/raspberry-pi-pico/nsbackend-esp32s3/00-build.sh):
```bash
cd ~/cbin/src/raspberry-pi-pico/nsbackend-esp32s3
./00-build.sh
```

### 3. Flash to ESP32-S3
Run [`./01-install.sh`](file:///home/omakoto/cbin/src/raspberry-pi-pico/nsbackend-esp32s3/01-install.sh):
```bash
./01-install.sh
# or specify the port explicitly:
./01-install.sh /dev/ttyACM0
```
