#!/usr/bin/env circuit-run
#file: ../libs/common.py
#file: ../libs/adafruit_mlx90640.py
#file: config.toml
#file: config-override.toml
#
# Kitchen Stove Heat Monitor for CircuitPython (ESP32-S3 / Seeed Studio XIAO ESP32-S3)
#
# Monitors kitchen stove surface temperature using an MLX90640 32x24 IR thermal camera.
# - Renders 32x24 temperature matrix over default serial (USB CDC) with ANSI heat-map colors.
# - Tracks active heat duration and sends Pushover alerts over Wi-Fi when a burner is left on
#   longer than the configured threshold duration (e.g. 15 minutes).
# - Uses native socketpool and ssl directly for HTTP/HTTPS alerts (zero external library dependencies).
# - Supports multi-AP Wi-Fi fallback, configurable HTTP/HTTPS endpoints, and TOML config files.

import gc
import ssl
import sys
import time
import board
import busio
import digitalio
import wifi
import socketpool

# Library imports
from common import get_i2c, get_led_pin, get_pin
import adafruit_mlx90640

# Configuration file locations on the CircuitPython filesystem
CONFIG_FILE_PATH: str = "config.toml"
CONFIG_OVERRIDE_FILE_PATH: str = "config-override.toml"

# Status LED State machine
class LedState:
    OFF: int = 0
    INITIALIZING: int = 1
    MONITORING_COOL: int = 2
    MONITORING_HOT: int = 3
    ERROR: int = 4


# Manages board LED with state-based blink patterns
class StatusLed:

    def __init__(self, active_low: bool = False) -> None:
        self.active_low: bool = active_low
        self.io: digitalio.DigitalInOut | None = None
        led_pin: board.Pin | None = get_led_pin()

        if led_pin is not None:
            try:
                self.io = digitalio.DigitalInOut(led_pin)
                self.io.direction = digitalio.Direction.OUTPUT
                self._set_raw(False)
            except Exception:
                self.io = None

        self.state: int = LedState.OFF
        self._pattern_start: float = time.monotonic()

    def _set_raw(self, turn_on: bool) -> None:
        if self.io is not None:
            self.io.value = not turn_on if self.active_low else turn_on

    def set_state(self, state: int) -> None:
        if self.state != state:
            self.state = state
            self._pattern_start = time.monotonic()

    def update(self) -> None:
        if self.io is None:
            return

        now: float = time.monotonic()
        elapsed: float = now - self._pattern_start

        if self.state == LedState.OFF:
            self._set_raw(False)
        elif self.state == LedState.INITIALIZING:
            # Fast blink 0.1s ON, 0.1s OFF
            cycle: float = elapsed % 0.2
            self._set_raw(cycle < 0.1)
        elif self.state == LedState.MONITORING_COOL:
            # Heartbeat pulse: brief 50ms pulse every 2 seconds
            cycle: float = elapsed % 2.0
            self._set_raw(cycle < 0.05)
        elif self.state == LedState.MONITORING_HOT:
            # Rapid warning pulse: 0.25s ON, 0.25s OFF
            cycle: float = elapsed % 0.5
            self._set_raw(cycle < 0.25)
        elif self.state == LedState.ERROR:
            # Steady slow blink: 0.5s ON, 0.5s OFF
            cycle: float = elapsed % 1.0
            self._set_raw(cycle < 0.5)


# Parses key-value pairs from a simple TOML configuration file
def parse_toml_file(file_path: str, config: dict[str, str | int | float | bool]) -> bool:
    try:
        with open(file_path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                if "=" in line:
                    key_part, val_part = line.split("=", 1)
                    key: str = key_part.strip()
                    val: str = val_part.strip()

                    # Strip trailing comments if not quoted
                    if "#" in val and not ((val.startswith('"') and val.endswith('"')) or (val.startswith("'") and val.endswith("'"))):
                        val = val.split("#", 1)[0].strip()

                    if (val.startswith('"') and val.endswith('"')) or (val.startswith("'") and val.endswith("'")):
                        config[key] = val[1:-1]
                    elif val.isdigit() or (val.startswith("-") and val[1:].isdigit()):
                        config[key] = int(val)
                    elif val.lower() == "true":
                        config[key] = True
                    elif val.lower() == "false":
                        config[key] = False
                    else:
                        # Attempt float parse
                        try:
                            config[key] = float(val)
                        except ValueError:
                            config[key] = val
        return True
    except OSError:
        return False


# Loads primary config.toml and overrides with config-override.toml if present
def load_toml_config(file_path: str = CONFIG_FILE_PATH, override_path: str = CONFIG_OVERRIDE_FILE_PATH) -> dict[str, str | int | float | bool]:
    config: dict[str, str | int | float | bool] = {}
    if parse_toml_file(file_path, config):
        print(f"Loaded config from '{file_path}'")
    if parse_toml_file(override_path, config):
        print(f"Loaded override config from '{override_path}'")
    return config


# Wi-Fi disconnect error mapping for human-readable diagnostics
WIFI_DISCONNECT_REASONS: dict[int, str] = {
    1: "Unspecified failure (General connection error)",
    2: "Auth expired (AP timed out during authentication)",
    3: "Auth leave (Disconnected by AP)",
    4: "Assoc expired (AP timed out during association)",
    5: "Assoc too many (Max client limit reached on AP)",
    15: "4-way handshake timeout (WRONG PASSWORD or weak signal)",
    201: "No AP found (SSID not found - verify 2.4 GHz band)",
    202: "Auth failed (WRONG PASSWORD)",
    204: "Handshake timeout (WRONG PASSWORD or weak signal)",
}


def format_wifi_error(e: Exception) -> str:
    err_str: str = str(e)
    if "Unknown failure" in err_str:
        words: list[str] = err_str.split()
        for i, word in enumerate(words):
            if word == "failure" and i + 1 < len(words):
                code_str: str = words[i + 1].strip(".:;,()")
                if code_str.isdigit():
                    code: int = int(code_str)
                    reason: str | None = WIFI_DISCONNECT_REASONS.get(code)
                    if reason:
                        return f"Unknown failure {code} ({reason})"
    return err_str


# Multi-AP Wi-Fi Connection Manager
class WifiManager:

    def __init__(self, config: dict[str, str | int | float | bool]) -> None:
        self.ap_list: list[tuple[str, str]] = []

        ssid0: str = str(config.get("wifi_ssid") or config.get("wifi_ssid0") or "").strip()
        pass0: str = str(config.get("wifi_password") or config.get("wifi_password0") or "")
        if ssid0:
            self.ap_list.append((ssid0, pass0))

        for i in range(1, 10):
            ssid_key: str = f"wifi_ssid{i}"
            pass_key: str = f"wifi_password{i}"
            ssid: str = str(config.get(ssid_key, "")).strip()
            password: str = str(config.get(pass_key, ""))
            if ssid:
                self.ap_list.append((ssid, password))

    @property
    def configured_ssids(self) -> list[str]:
        return [ssid for ssid, _ in self.ap_list]

    def scan_networks(self) -> set[str]:
        print("Scanning visible Wi-Fi networks...")
        visible_ssids: set[str] = set()
        try:
            count: int = 0
            for net in wifi.radio.start_scanning_networks():
                ssid_name: str = net.ssid if net.ssid else ""
                display_name: str = ssid_name if ssid_name else "<hidden>"
                print(f"  [AP] SSID: '{display_name}', RSSI: {net.rssi} dBm, Ch: {net.channel}")
                if ssid_name:
                    visible_ssids.add(ssid_name)
                count += 1
            if count == 0:
                print("  No visible Wi-Fi networks found.")
        except Exception as e:
            print(f"  Wi-Fi scan failed: {e}")
        finally:
            try:
                wifi.radio.stop_scanning_networks()
            except Exception:
                pass
        return visible_ssids

    def _attempt_connect(self, ssid: str, password: str) -> bool:
        print(f"Connecting to Wi-Fi SSID: '{ssid}'...")
        try:
            wifi.radio.connect(ssid, password)
            print(f"Connected to Wi-Fi! IP: {wifi.radio.ipv4_address}")
            return True
        except Exception as e:
            err_msg: str = format_wifi_error(e)
            print(f"Wi-Fi connection to '{ssid}' failed: {err_msg}")
            return False

    def connect(self, led: StatusLed | None = None) -> None:
        if wifi.radio.connected:
            return

        if not self.ap_list:
            print("Warning: No Wi-Fi SSIDs configured.")
            return

        if led is not None:
            led.set_state(LedState.INITIALIZING)

        last_tried_idx: int = 0
        first_ssid, first_pass = self.ap_list[0]
        if self._attempt_connect(first_ssid, first_pass):
            return

        while not wifi.radio.connected:
            if led is not None:
                led.update()

            visible_ssids: set[str] = self.scan_networks()
            candidates: list[tuple[int, str, str]] = []
            num_aps: int = len(self.ap_list)

            for step in range(1, num_aps + 1):
                idx: int = (last_tried_idx + step) % num_aps
                ssid, password = self.ap_list[idx]
                if ssid in visible_ssids:
                    candidates.append((idx, ssid, password))

            if not candidates:
                print("No configured Wi-Fi APs visible in scan. Retrying in 3 seconds...")
                time.sleep(3.0)
                continue

            for idx, ssid, password in candidates:
                if led is not None:
                    led.update()
                last_tried_idx = idx
                if self._attempt_connect(ssid, password):
                    return

            print("All visible Wi-Fi candidates failed. Retrying scan in 3 seconds...")
            time.sleep(3.0)


# URL helper to parse protocol, host, port, and path without urllib
def parse_url(url: str) -> tuple[str, str, int, str]:
    proto: str
    rest: str
    if "://" in url:
        proto, rest = url.split("://", 1)
    else:
        proto, rest = "http", url

    path: str
    if "/" in rest:
        host_part, path_part = rest.split("/", 1)
        path = "/" + path_part
    else:
        host_part = rest
        path = "/"

    host: str
    port: int
    if ":" in host_part:
        host, port_str = host_part.split(":", 1)
        port = int(port_str)
    else:
        host = host_part
        port = 443 if proto.lower() == "https" else 80

    return proto.lower(), host, port, path


# Helper to URL-encode form field values without urllib
def urlencode_val(val: str | int | float) -> str:
    res: list[str] = []
    for ch in str(val):
        if ch.isalnum() or ch in "-_.~":
            res.append(ch)
        elif ch == " ":
            res.append("+")
        else:
            res.append(f"%{ord(ch):02X}")
    return "".join(res)


# Native zero-dependency Pushover client using built-in socketpool and ssl
class PushoverNotifier:

    def __init__(self, token: str, user_key: str, url: str) -> None:
        self.token: str = token
        self.user_key: str = user_key
        self.url: str = url

    def send_notification(self, title: str, message: str, priority: int = 0) -> bool:
        if not self.token or not self.user_key or self.token == "YOUR_PUSHOVER_APP_TOKEN":
            print(f"[Pushover] Skipped (API token or user key not configured): {title} - {message}")
            return False

        if not wifi.radio.connected:
            print("[Pushover] Cannot send notification: Wi-Fi disconnected.")
            return False

        proto, host, port, path = parse_url(self.url)
        print(f"[Pushover] Sending notification via {proto.upper()} to {host}:{port}{path}...")

        payload_dict: dict[str, str | int] = {
            "token": self.token,
            "user": self.user_key,
            "title": title,
            "message": message,
            "priority": priority,
        }
        body: str = "&".join(f"{k}={urlencode_val(v)}" for k, v in payload_dict.items())
        body_bytes: bytes = body.encode("utf-8")

        start_t: float = time.monotonic()
        sock = None
        try:
            pool = socketpool.SocketPool(wifi.radio)
            addr_info = pool.getaddrinfo(host, port)
            addr = addr_info[0][4]

            raw_sock = pool.socket(pool.AF_INET, pool.SOCK_STREAM)
            raw_sock.settimeout(10.0)

            if proto == "https":
                ssl_ctx = ssl.create_default_context()
                sock = ssl_ctx.wrap_socket(raw_sock, server_hostname=host)
            else:
                sock = raw_sock

            sock.connect(addr)
            req_header = (
                f"POST {path} HTTP/1.1\r\n"
                f"Host: {host}\r\n"
                f"User-Agent: CircuitPython-ESP32S3\r\n"
                f"Content-Type: application/x-www-form-urlencoded\r\n"
                f"Content-Length: {len(body_bytes)}\r\n"
                f"Connection: close\r\n\r\n"
            )
            sock.send(req_header.encode("utf-8") + body_bytes)

            # Read response
            resp_bytes = bytearray()
            buf = bytearray(512)
            while True:
                num = sock.recv_into(buf)
                if num == 0:
                    break
                resp_bytes.extend(buf[:num])
                if b"\r\n\r\n" in resp_bytes:
                    break

            sock.close()
            sock = None
            elapsed: float = time.monotonic() - start_t

            header_text = resp_bytes.split(b"\r\n\r\n")[0].decode("utf-8", errors="replace")
            status_line = header_text.split("\r\n")[0] if header_text else ""

            if " 200 " in status_line or status_line.endswith(" 200"):
                print(f"[Pushover] Success ({status_line}, took {elapsed:.2f}s)")
                return True
            else:
                print(f"[Pushover] Server error: {status_line}")
                return False

        except Exception as e:
            print(f"[Pushover] Failed to send alert: {e}")
            if sock is not None:
                try:
                    sock.close()
                except Exception:
                    pass
            return False


# Initializes I2C bus respecting configuration overrides and board defaults
def create_i2c_bus(scl_gpio: int | None, sda_gpio: int | None, frequency: int) -> busio.I2C:
    # Explicit SoC GPIO numbers
    if scl_gpio is not None and sda_gpio is not None:
        print(f"Initializing I2C on explicit GPIO SCL={scl_gpio}, SDA={sda_gpio} at {frequency}Hz")
        return busio.I2C(scl=get_pin(scl_gpio), sda=get_pin(sda_gpio), frequency=frequency)

    # Board default pins (e.g. XIAO ESP32-S3 D4/D5 or generic ESP32-S3 SDA/SCL)
    if hasattr(board, "SCL") and hasattr(board, "SDA"):
        try:
            print(f"Initializing I2C on default board.SCL / board.SDA at {frequency}Hz")
            return busio.I2C(scl=board.SCL, sda=board.SDA, frequency=frequency)
        except Exception as e:
            print(f"Default board.SCL/SDA init failed ({e}), falling back to common.get_i2c()")

    # General fallback resolution
    return get_i2c(scl=scl_gpio, sda=sda_gpio)


# Prints 32x24 thermal matrix to serial with ANSI temperature color codes
def render_matrix_to_serial(frame: list[float], threshold_c: float, colorize: bool) -> None:
    # ANSI color definitions
    # Blue: < 30°C
    # Green: 30°C - 45°C
    # Yellow: 45°C - threshold_c
    # Red background: >= threshold_c
    color_reset: str = "\033[0m" if colorize else ""
    color_blue: str = "\033[94m" if colorize else ""
    color_green: str = "\033[92m" if colorize else ""
    color_yellow: str = "\033[93m" if colorize else ""
    color_hot: str = "\033[97;41m" if colorize else ""  # White on Red background

    print("\n" + "=" * 100)
    print("      MLX90640 Thermal Image (32 columns x 24 rows) [°C]")
    print("-" * 100)

    for row in range(24):
        row_str_parts: list[str] = [f"{row:02d} | "]
        for col in range(32):
            idx: int = row * 32 + col
            val: float = frame[idx]

            color: str = ""
            if colorize:
                if val >= threshold_c:
                    color = color_hot
                elif val >= 45.0:
                    color = color_yellow
                elif val >= 30.0:
                    color = color_green
                else:
                    color = color_blue

            # Format as 2-digit rounded integer with leading space
            int_val: int = int(round(val))
            row_str_parts.append(f"{color}{int_val:2d}{color_reset} ")

        print("".join(row_str_parts))
    print("=" * 100)


# Main Application Logic
def main() -> None:
    print("Starting Kitchen Stove Heat Monitor...")

    # Load configuration
    config: dict[str, str | int | float | bool] = load_toml_config()

    # Alert thresholds
    threshold_c: float = float(config.get("threshold_c", 60.0))
    hysteresis_c: float = float(config.get("hysteresis_c", 5.0))
    alert_delay_min: float = float(config.get("alert_delay_minutes", 15.0))
    alert_repeat_min: float = float(config.get("alert_repeat_minutes", 15.0))
    notify_on_cooldown: bool = bool(config.get("notify_on_cooldown", True))

    # Timing and display
    monitoring_interval_s: float = float(config.get("monitoring_interval_s", 2.0))
    dump_matrix: bool = bool(config.get("dump_matrix", True))
    color_matrix: bool = bool(config.get("color_matrix", True))

    # Hardware settings
    i2c_frequency: int = int(config.get("i2c_frequency", 400000))
    scl_gpio: int | None = int(config["i2c_scl"]) if "i2c_scl" in config else None
    sda_gpio: int | None = int(config["i2c_sda"]) if "i2c_sda" in config else None
    led_active_low: bool = bool(config.get("led_active_low", False))

    # Pushover credentials
    pushover_token: str = str(config.get("pushover_token", "")).strip()
    pushover_user: str = str(config.get("pushover_user_key", "")).strip()
    pushover_url: str = str(config.get("pushover_url", "https://api.pushover.net/1/messages.json")).strip()

    # Initialize status LED
    led: StatusLed = StatusLed(active_low=led_active_low)
    led.set_state(LedState.INITIALIZING)
    led.update()

    # Connect to Wi-Fi
    wifi_manager: WifiManager = WifiManager(config)
    print(f"Configured Wi-Fi SSIDs: {wifi_manager.configured_ssids}")
    wifi_manager.connect(led)

    # Initialize Pushover client
    pushover: PushoverNotifier = PushoverNotifier(
        token=pushover_token,
        user_key=pushover_user,
        url=pushover_url
    )

    # Initialize I2C Bus & MLX90640 Camera
    print("Initializing MLX90640 thermal camera...")
    try:
        i2c = create_i2c_bus(scl_gpio, sda_gpio, i2c_frequency)
        mlx = adafruit_mlx90640.MLX90640(i2c)
        mlx.refresh_rate = adafruit_mlx90640.RefreshRate.REFRESH_2_HZ
        print("MLX90640 sensor detected and initialized successfully!")
    except Exception as e:
        print(f"Error initializing MLX90640 sensor: {e}")
        led.set_state(LedState.ERROR)
        while True:
            led.update()
            time.sleep(0.5)

    # Buffer for 768 pixels (32 cols x 24 rows)
    frame: list[float] = [0.0] * 768

    # State tracking variables
    is_hot: bool = False
    consecutive_hot_frames: int = 0
    hot_start_time: float = 0.0
    alert_sent: bool = False
    last_alert_time: float = 0.0

    print("\nEntering monitoring loop...")
    print(f"Parameters: Threshold={threshold_c:.1f}°C, Alert Delay={alert_delay_min:.1f} min, Interval={monitoring_interval_s:.1f}s\n")

    led.set_state(LedState.MONITORING_COOL)

    while True:
        led.update()

        # Capture thermal frame
        try:
            mlx.getFrame(frame)
        except (ValueError, RuntimeError) as e:
            # MLX90640 I2C reads occasionally report frame errors; skip and retry
            time.sleep(0.1)
            continue

        now: float = time.monotonic()
        min_temp: float = min(frame)
        max_temp: float = max(frame)
        avg_temp: float = sum(frame) / 768.0

        # Dump temperature matrix to UART serial if configured
        if dump_matrix:
            render_matrix_to_serial(frame, threshold_c, color_matrix)

        # Evaluate stove temperature condition
        if max_temp >= threshold_c:
            consecutive_hot_frames += 1
            # Require 2 consecutive frames to reject single-frame electrical noise
            if not is_hot and consecutive_hot_frames >= 2:
                is_hot = True
                hot_start_time = now
                alert_sent = False
                last_alert_time = 0.0
                led.set_state(LedState.MONITORING_HOT)
                print(f"\n[HOT DETECTED] Stove surface max temp: {max_temp:.1f}°C (Threshold: {threshold_c:.1f}°C). Timer started.")

            if is_hot:
                hot_elapsed_s: float = now - hot_start_time
                hot_elapsed_min: float = hot_elapsed_s / 60.0

                print(f"[STATUS: HOT] Elapsed: {hot_elapsed_min:.1f} min | Max: {max_temp:.1f}°C | Min: {min_temp:.1f}°C | Avg: {avg_temp:.1f}°C")

                # Check if stove has stayed hot beyond the alert delay
                if not alert_sent and hot_elapsed_min >= alert_delay_min:
                    msg: str = (
                        f"Stove surface has been HOT (max {max_temp:.1f}°C, threshold {threshold_c:.0f}°C) "
                        f"for {hot_elapsed_min:.0f} minutes!"
                    )
                    print(f"\n[ALERT TRIGGERED] {msg}")
                    if pushover.send_notification(
                        title="Stove Left ON Warning!",
                        message=msg,
                        priority=1  # High priority alert
                    ):
                        alert_sent = True
                        last_alert_time = now

                # Re-notify if stove continues to remain hot after initial alert
                elif alert_sent and alert_repeat_min > 0:
                    since_last_alert_s: float = now - last_alert_time
                    if since_last_alert_s >= (alert_repeat_min * 60.0):
                        msg = (
                            f"Stove is STILL HOT (max {max_temp:.1f}°C) after {hot_elapsed_min:.0f} minutes! "
                            f"Please check your kitchen stove."
                        )
                        print(f"\n[REPEAT ALERT] {msg}")
                        if pushover.send_notification(
                            title="Stove STILL HOT Warning!",
                            message=msg,
                            priority=1
                        ):
                            last_alert_time = now

        else:
            # Below threshold
            consecutive_hot_frames = 0
            if is_hot:
                # Check hysteresis before declaring cool
                if max_temp < (threshold_c - hysteresis_c):
                    hot_elapsed_min = (now - hot_start_time) / 60.0
                    print(f"\n[COOLED DOWN] Stove temperature dropped to {max_temp:.1f}°C. Was hot for {hot_elapsed_min:.1f} min.")

                    if alert_sent and notify_on_cooldown:
                        cooldown_msg: str = (
                            f"Stove has cooled down to {max_temp:.1f}°C (below {threshold_c - hysteresis_c:.0f}°C). "
                            f"Total hot duration was {hot_elapsed_min:.0f} minutes."
                        )
                        pushover.send_notification(
                            title="Stove Cooled Down",
                            message=cooldown_msg,
                            priority=0  # Normal priority
                        )

                    is_hot = False
                    alert_sent = False
                    hot_start_time = 0.0
                    led.set_state(LedState.MONITORING_COOL)
            else:
                print(f"[STATUS: COOL] Max: {max_temp:.1f}°C | Min: {min_temp:.1f}°C | Avg: {avg_temp:.1f}°C")

        # Periodically invoke garbage collection to preserve memory headroom
        gc.collect()

        # Sleep for remainder of monitoring interval
        time.sleep(monitoring_interval_s)


if __name__ == "__main__":
    main()
