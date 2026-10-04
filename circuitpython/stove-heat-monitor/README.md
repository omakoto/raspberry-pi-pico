# Kitchen Stove Heat Monitor

A CircuitPython application running on the **ESP32-S3** (compatible with generic dev boards and the Seeed Studio XIAO ESP32-S3) that monitors kitchen cooktop surface temperatures using the **MLX90640** 32×24 infrared thermal imaging sensor.

The monitor helps prevent fire hazards and energy waste by detecting when a stove is accidentally left turned on. To avoid false alarms during normal, intentional cooking, it triggers a **Pushover notification** only after any surface area has remained hot ($\ge 60^\circ\text{C}$ by default) for longer than a configurable duration (e.g. 15 minutes).

---

## Features

- **Pushover Push Notifications**: Sends immediate mobile alerts when a burner is left on unattended, plus optional cooldown notices when the stove turns off.
- **Configurable HTTP/HTTPS Endpoint**: Works out of the box with official Pushover HTTPS API, and can easily point to a local LAN HTTP proxy or webhook relay.
- **32×24 Matrix Thermal Visualizer**: Streams the full temperature matrix over default USB serial (`print()`), colorized with ANSI codes to spot hot zones in the terminal.
- **Robust Multi-AP Wi-Fi**: Reuses the network configuration engine from `nsbackend-pico`, automatically scanning visible SSIDs and falling back between primary and secondary access points.
- **Non-Destructive Overrides**: Keep defaults in `config.toml` and maintain your private Wi-Fi passwords and API keys in `config-override.toml`.

---

## Hardware & Wiring

### MLX90640 Sensor Pinout

| Sensor Pin | ESP32-S3 Dev Board | Seeed Studio XIAO ESP32-S3 | Notes |
| :--- | :--- | :--- | :--- |
| **VIN / VDD** | **3.3V** *(Either 3.3V pin)* | **3V3** (Pin 12) | Connects to 3.3V power output |
| **GND** | **GND** | **GND** (Pin 13) | Common ground |
| **SDA** | **SDA** / `GPIO5` | **D4** (Pin 5, `GPIO5`) | Hardware I2C Data line |
| **SCL** | **SCL** / `GPIO6` | **D5** (Pin 6, `GPIO6`) | Hardware I2C Clock line |

> [!NOTE]
> **Dev Board 3.3V Pins**: Generic ESP32-S3 dev boards (such as DevKitC-1) typically provide two pins labeled `3V3` or `3.3V`. Both pins are internally tied to the same on-board 3.3V LDO voltage regulator rail. You can connect your sensor's power wire to **either** of the two pins interchangeably.

> [!WARNING]
> **Enclosure Warning**: Standard glass, clear acrylic, or polycarbonate lenses completely block far-infrared thermal radiation (8–14 µm). If mounting in an enclosure or range-hood housing, keep the sensor aperture open to the air (using a shroud/baffle) or use an IR-transparent window (e.g. thin polyethylene film or Germanium).

---

## Configuration

Settings are loaded first from [`config.toml`](config.toml) and then overridden by `config-override.toml` (if present).

To configure private credentials, copy `config-override.toml.example` to `config-override.toml`:

```toml
wifi_ssid = "YourWiFiSSID"
wifi_password = "YourWiFiPassword"

# Pushover API settings
pushover_token = "YOUR_APPLICATION_API_TOKEN"
pushover_user_key = "YOUR_USER_KEY"

# Alert detection parameters
threshold_c = 60.0
alert_delay_minutes = 15.0
alert_repeat_minutes = 15.0
notify_on_cooldown = true

# Serial matrix output
dump_matrix = true
color_matrix = true
```

### Parameter Reference

| Parameter | Type | Default | Description |
| :--- | :--- | :--- | :--- |
| `wifi_ssid` | `str` | `"WIFI-SSID"` | Primary Wi-Fi network SSID |
| `wifi_password` | `str` | `"PASSWORD"` | Primary Wi-Fi network password |
| `wifi_ssid1`..`9` | `str` | `""` | Optional fallback Wi-Fi networks |
| `pushover_token` | `str` | `""` | Pushover application API token |
| `pushover_user_key` | `str` | `""` | Pushover user key |
| `pushover_url` | `str` | `https://api.pushover.net/1/messages.json` | Pushover endpoint (HTTP or HTTPS) |
| `threshold_c` | `float` | `60.0` | Temperature in °C considered active/hot |
| `hysteresis_c` | `float` | `5.0` | Drop below threshold required for cooldown |
| `alert_delay_minutes`| `float` | `15.0` | Minutes continuously hot before alerting |
| `alert_repeat_minutes`| `float`| `15.0` | Repeat interval in minutes while remaining hot |
| `notify_on_cooldown`| `bool` | `true` | Send notification when stove cools down |
| `monitoring_interval_s`| `float`| `2.0` | Seconds between thermal reads |
| `dump_matrix` | `bool` | `true` | Stream 32×24 matrix to UART serial |
| `color_matrix` | `bool` | `true` | Use ANSI colors in matrix dump |
| `i2c_frequency` | `int` | `400000` | I2C clock frequency (400 kHz) |
| `i2c_scl` / `sda` | `int` | `None` | Optional explicit SoC GPIO numbers |

---

## How to Run

1. Connect your ESP32-S3 board via USB.
2. Run using `circuit-run`:
   ```bash
   ./stove-heat-monitor.py
   ```
   `circuit-run` automatically copies `stove-heat-monitor.py` (as `code.py`), `config.toml`, `config-override.toml`, and the required libraries (`libs/common.py` and `libs/adafruit_mlx90640.py`) to the board, then opens the serial monitor (`tools/monitor.sh`).

3. To view the colorized thermal matrix output directly in your terminal, make sure your terminal supports ANSI color codes (standard in Linux/macOS terminals).
