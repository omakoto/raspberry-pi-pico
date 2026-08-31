#!/usr/bin/env circuit-run
#file: boot.py
#file: config.toml
#
# Nintendo Switch Controller TCP Backend for CircuitPython (ESP32 / Pico 2 W).
#
# Emulates a HORI Pokken Nintendo Switch controller over USB HID composite gadget mode
# and runs a TCP server on port 10100 (advertised as nscon.local via mDNS).
# Receives controller commands streamed from nsfrontend or interactive pipelines,
# parsing inputs, directional sticks, and auto-releases, then updates HID reports.
# Uses the onboard user LED to indicate progress:
#   - Initialization (Wi-Fi + Server startup): LED constantly ON
#   - Waiting for client: 3 blinks (0.2s on, 0.2s off x 3, 0.5s pause)
#   - Client connected: Heartbeat (1.0s on, 1.0s off)

import math
import struct
import time
import board
import digitalio
import mdns
import socketpool
import usb_hid
import wifi

# Pin Configuration Constants
PIN_LED: board.Pin | None = getattr(board, "LED", None)

# Configuration file locations on the CircuitPython filesystem
CONFIG_FILE_PATH: str = "config.toml"
CONFIG_BASE_FILE_PATH: str = "config-base.toml"

# Nintendo Switch HID Constants
HID_USAGE_PAGE_GENERIC: int = 0x01
HID_USAGE_GAMEPAD: int = 0x05

# Button Bitmasks (16-bit uint16 little-endian)
BTN_NONE: int = 0x0000
BTN_Y: int = 1 << 0
BTN_B: int = 1 << 1
BTN_A: int = 1 << 2
BTN_X: int = 1 << 3
BTN_L: int = 1 << 4
BTN_R: int = 1 << 5
BTN_ZL: int = 1 << 6
BTN_ZR: int = 1 << 7
BTN_MINUS: int = 1 << 8
BTN_PLUS: int = 1 << 9
BTN_LSTICK: int = 1 << 10
BTN_RSTICK: int = 1 << 11
BTN_HOME: int = 1 << 12
BTN_CAPTURE: int = 1 << 13

# Hat Switch (D-Pad) Values
HAT_UP: int = 0x00
HAT_UP_RIGHT: int = 0x01
HAT_RIGHT: int = 0x02
HAT_DOWN_RIGHT: int = 0x03
HAT_DOWN: int = 0x04
HAT_DOWN_LEFT: int = 0x05
HAT_LEFT: int = 0x06
HAT_UP_LEFT: int = 0x07
HAT_CENTER: int = 0x08

# Default auto-release duration for single-token commands
DEFAULT_AUTO_RELEASE_SECONDS: float = 0.050


# Progress LED step states
class LedState:
    INITIALIZING: int = 1     # Startup (Wi-Fi + Server setup): LED constantly ON
    WAITING_CLIENT: int = 2   # Waiting for client: 3 blinks (0.2s ON, 0.2s OFF, 0.2s ON, 0.2s OFF, 0.2s ON) - 0.5s pause
    CLIENT_CONNECTED: int = 3 # Client connected: 1.0s ON, 1.0s OFF


# Status LED indicator for system lifecycle states
class StatusLed:

    def __init__(self, active_low: bool = False) -> None:
        self.io: digitalio.DigitalInOut | None = None
        self.active_low: bool = active_low
        if PIN_LED is not None:
            try:
                self.io = digitalio.DigitalInOut(PIN_LED)
                self.io.direction = digitalio.Direction.OUTPUT
                print(f"Status LED initialized on board.LED (active_low={self.active_low})")
            except Exception as e:
                print(f"Warning: Failed to initialize digitalio on board.LED: {e}")
        else:
            print("Warning: board.LED is not defined on this board")

        self.current_step: int = LedState.INITIALIZING
        self.step_start_time: float = time.monotonic()
        self.update()

    def _set_raw(self, state: bool) -> None:
        if self.io is not None:
            self.io.value = (not state) if self.active_low else state

    def on(self) -> None:
        self._set_raw(True)

    def off(self) -> None:
        self._set_raw(False)

    def set_step(self, step: int) -> None:
        if self.current_step != step:
            self.current_step = step
            self.step_start_time = time.monotonic()
        self.update()

    def update(self) -> None:
        if self.io is None:
            return

        now = time.monotonic()
        elapsed = now - self.step_start_time

        if self.current_step == LedState.INITIALIZING:
            # Constantly ON during startup & connection
            self._set_raw(True)

        elif self.current_step == LedState.WAITING_CLIENT:
            # 3 blinks (0.2s ON, 0.2s OFF, 0.2s ON, 0.2s OFF, 0.2s ON) - 0.5s pause (Cycle: 1.5s)
            cycle_time = elapsed % 1.5
            if cycle_time < 0.2:
                self._set_raw(True)
            elif cycle_time < 0.4:
                self._set_raw(False)
            elif cycle_time < 0.6:
                self._set_raw(True)
            elif cycle_time < 0.8:
                self._set_raw(False)
            elif cycle_time < 1.0:
                self._set_raw(True)
            else:
                self._set_raw(False)

        elif self.current_step == LedState.CLIENT_CONNECTED:
            # 1.0s ON, 1.0s OFF (Cycle: 2.0s)
            cycle_time = elapsed % 2.0
            self._set_raw(cycle_time < 1.0)


# Parse simple key-value pairs from a TOML file into the config dictionary
def parse_toml_file(file_path: str, config: dict[str, str | int | bool]) -> bool:
    try:
        with open(file_path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                if "=" in line:
                    key_part, val_part = line.split("=", 1)
                    key = key_part.strip()
                    val = val_part.strip()

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
                        config[key] = val
        return True
    except OSError:
        return False


# Load TOML configuration, reading main config first and overriding with config-base.toml if present
def load_toml_config(file_path: str = CONFIG_FILE_PATH, base_path: str = CONFIG_BASE_FILE_PATH) -> dict[str, str | int | bool]:
    config: dict[str, str | int | bool] = {}
    if parse_toml_file(file_path, config):
        print(f"Loaded config from '{file_path}'")
    if parse_toml_file(base_path, config):
        print(f"Loaded override config from '{base_path}'")
    return config


# Human-readable Wi-Fi disconnect reason descriptions (mapping ESP-IDF and CYW43 error codes)
WIFI_DISCONNECT_REASONS: dict[int, str] = {
    1: "Unspecified failure (General connection error)",
    2: "Auth expired (AP timed out during authentication - check if SSID is on 2.4 GHz, signal strength, or AP MAC filtering)",
    3: "Auth leave (Disconnected by AP)",
    4: "Assoc expired (AP timed out during association - check 2.4 GHz band compatibility)",
    5: "Assoc too many (AP rejected connection: Max client limit reached on AP)",
    6: "Not authenticated",
    7: "Not associated",
    8: "Assoc leave (Disassociated by AP)",
    9: "Assoc not authed",
    10: "Disassoc power cap bad",
    11: "Disassoc supported channels bad",
    13: "Invalid Information Element",
    14: "MIC failure (WPA encryption mismatch / corrupted frames)",
    15: "4-way handshake timeout (WRONG PASSWORD, weak signal, or WPA3/PMF mismatch)",
    16: "Group key update timeout",
    17: "IE in 4-way differs",
    18: "Invalid group cipher (Check AP security settings: WPA2-PSK recommended)",
    19: "Invalid pairwise cipher (Check AP security settings: AES/CCMP recommended)",
    20: "Invalid AKMP (Authentication Key Management Protocol mismatch)",
    21: "Unsupported RSN IE version",
    22: "Invalid RSN IE capabilities",
    23: "802.1X auth failed (Enterprise auth not supported)",
    24: "Cipher suite rejected",
    200: "Beacon timeout (Lost connection to AP)",
    201: "No AP found (SSID not found - verify SSID spelling and ensure 2.4 GHz band is active)",
    202: "Auth failed (WRONG PASSWORD)",
    203: "Assoc failed",
    204: "Handshake timeout (WRONG PASSWORD or weak signal)",
    205: "Connection failed (General radio/handshake failure, weak signal, or WPA3 mismatch)",
}


# Formats Wi-Fi connection exceptions into actionable error descriptions
def format_wifi_error(e: Exception) -> str:
    err_str = str(e)
    if "Unknown failure" in err_str:
        words = err_str.split()
        for i, word in enumerate(words):
            if word == "failure" and i + 1 < len(words):
                code_str = words[i + 1].strip(".:;,()")
                if code_str.isdigit():
                    code = int(code_str)
                    reason = WIFI_DISCONNECT_REASONS.get(code)
                    if reason:
                        return f"Unknown failure {code} ({reason})"
    return err_str


# Manages Wi-Fi network configuration and connections with multi-AP fallback and scan-based selection
class WifiManager:

    def __init__(self, config: dict[str, str | int | bool]) -> None:
        self.ap_list: list[tuple[str, str]] = []

        # Index 0: Check 'wifi_ssid' / 'wifi_password', accepting 'wifi_ssid0' / 'wifi_password0' as aliases
        ssid0 = str(config.get("wifi_ssid") or config.get("wifi_ssid0") or "").strip()
        pass0 = str(config.get("wifi_password") or config.get("wifi_password0") or "")
        if ssid0:
            self.ap_list.append((ssid0, pass0))

        # Indices 1 to 9: Check 'wifi_ssid1'..'wifi_ssid9' and 'wifi_password1'..'wifi_password9'
        for i in range(1, 10):
            ssid_key = f"wifi_ssid{i}"
            pass_key = f"wifi_password{i}"
            ssid = str(config.get(ssid_key, "")).strip()
            password = str(config.get(pass_key, ""))
            if ssid:
                self.ap_list.append((ssid, password))

    @property
    def configured_ssids(self) -> list[str]:
        return [ssid for ssid, _ in self.ap_list]

    def scan_networks(self) -> set[str]:
        # Scans visible Wi-Fi access points and prints diagnostic details
        print("Scanning visible Wi-Fi networks...")
        visible_ssids: set[str] = set()
        try:
            count = 0
            for net in wifi.radio.start_scanning_networks():
                ssid_name = net.ssid if net.ssid else ""
                display_name = ssid_name if ssid_name else "<hidden>"
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
        print(f"Connecting to Wi-Fi SSID: '{ssid}' (password length: {len(password)})...")
        try:
            wifi.radio.connect(ssid, password)
            print(f"Connected to Wi-Fi successfully! IP: {wifi.radio.ipv4_address}")
            return True
        except Exception as e:
            err_msg = format_wifi_error(e)
            print(f"Wi-Fi connection to '{ssid}' failed: {err_msg}")
            return False

    def connect(self, led: StatusLed | None = None) -> None:
        if wifi.radio.connected:
            print(f"Already connected to Wi-Fi. IP: {wifi.radio.ipv4_address}")
            return

        if not self.ap_list:
            print("Warning: No Wi-Fi SSIDs configured.")
            time.sleep(5.0)
            return

        if led is not None:
            led.set_step(LedState.INITIALIZING)

        # First attempt: Try the first configured AP directly
        last_tried_idx = 0
        first_ssid, first_pass = self.ap_list[0]
        if self._attempt_connect(first_ssid, first_pass):
            return

        # If the first attempt fails, scan visible networks and try next matching APs in scan result
        while not wifi.radio.connected:
            if led is not None:
                led.update()

            visible_ssids = self.scan_networks()

            # Find and try candidates in order starting after the last tried index
            candidates: list[tuple[int, str, str]] = []
            num_aps = len(self.ap_list)
            for step in range(1, num_aps + 1):
                idx = (last_tried_idx + step) % num_aps
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

            # If all candidates in this scan cycle failed, pause before re-scanning
            print("All visible Wi-Fi candidates failed. Retrying scan in 3 seconds...")
            time.sleep(3.0)


# Manages communication with the Nintendo Switch HID gamepad endpoint
class SwitchGamepad:

    def __init__(self) -> None:
        self._device: usb_hid.Device | None = None
        for dev in usb_hid.devices:
            if dev.usage_page == HID_USAGE_PAGE_GENERIC and dev.usage == HID_USAGE_GAMEPAD:
                self._device = dev
                break

        if self._device is None:
            raise RuntimeError(
                "Switch Gamepad HID device not found. Ensure boot.py is installed and board was reset."
            )

        self._last_report: bytes = b""

    def send_state(
        self,
        buttons: int,
        hat: int,
        lx: int,
        ly: int,
        rx: int,
        ry: int,
    ) -> None:
        # Pack 8-byte HID report
        report: bytes = struct.pack("<HBBBBBB", buttons, hat, lx, ly, rx, ry, 0x00)
        if report != self._last_report:
            for _ in range(10):
                try:
                    self._device.send_report(report)
                    self._last_report = report
                    return
                except OSError:
                    time.sleep(0.002)

            try:
                self._device.send_report(report)
                self._last_report = report
            except OSError:
                pass


# Represents full controller state and command execution
class ControllerState:

    def __init__(self, gamepad: SwitchGamepad) -> None:
        self.gamepad: SwitchGamepad = gamepad

        # Individual button states
        self.buttons: int = BTN_NONE

        # D-pad individual directions
        self.dpad_up: bool = False
        self.dpad_down: bool = False
        self.dpad_left: bool = False
        self.dpad_right: bool = False

        # Analog stick values: float -1.0 .. +1.0
        self.lx: float = 0.0
        self.ly: float = 0.0
        self.rx: float = 0.0
        self.ry: float = 0.0

        # Auto-release / delayed command queue: list of (target_monotonic_time, command_string)
        self.scheduled_commands: list[tuple[float, str]] = []

    def reset_all(self) -> None:
        # Resets all controller inputs to neutral released state
        self.buttons = BTN_NONE
        self.dpad_up = False
        self.dpad_down = False
        self.dpad_left = False
        self.dpad_right = False
        self.lx = 0.0
        self.ly = 0.0
        self.rx = 0.0
        self.ry = 0.0
        self.scheduled_commands.clear()
        self.sync_report()

    def sync_report(self) -> None:
        # Resolves hat direction with opposing cancellation
        up = self.dpad_up
        down = self.dpad_down
        left = self.dpad_left
        right = self.dpad_right

        if up and down:
            up = down = False
        if left and right:
            left = right = False

        if up:
            if right:
                hat = HAT_UP_RIGHT
            elif left:
                hat = HAT_UP_LEFT
            else:
                hat = HAT_UP
        elif down:
            if right:
                hat = HAT_DOWN_RIGHT
            elif left:
                hat = HAT_DOWN_LEFT
            else:
                hat = HAT_DOWN
        elif right:
            hat = HAT_RIGHT
        elif left:
            hat = HAT_LEFT
        else:
            hat = HAT_CENTER

        # Map analog floats [-1.0 .. +1.0] to byte [0 .. 255] with 128 as neutral center
        lx_byte = int(round(128.0 + (self.lx * 127.0)))
        lx_byte = max(0, min(255, lx_byte))

        # ly in Switch controller HID: 0 is UP, 255 is DOWN
        ly_byte = int(round(128.0 + (self.ly * 127.0)))
        ly_byte = max(0, min(255, ly_byte))

        rx_byte = int(round(128.0 + (self.rx * 127.0)))
        rx_byte = max(0, min(255, rx_byte))

        ry_byte = int(round(128.0 + (self.ry * 127.0)))
        ry_byte = max(0, min(255, ry_byte))

        self.gamepad.send_state(self.buttons, hat, lx_byte, ly_byte, rx_byte, ry_byte)

    def set_button(self, mask: int, state: bool) -> None:
        if state:
            self.buttons |= mask
        else:
            self.buttons &= ~mask

    def execute_command(self, cmd_line: str) -> None:
        # Strip comments and extra whitespace
        if "#" in cmd_line:
            cmd_line = cmd_line.split("#", 1)[0]
        cmd_line = cmd_line.strip()
        if not cmd_line:
            return

        tokens = cmd_line.split()
        if not tokens:
            return

        cmd_idx = 0
        duration_s = 0.0

        # Check for optional leading duration e.g. "0.05 a 1"
        first_token = tokens[0]
        if first_token and (first_token[0].isdigit() or first_token[0] == "."):
            try:
                duration_s = float(first_token)
                cmd_idx = 1
            except ValueError:
                pass

        if cmd_idx >= len(tokens):
            return

        cmd = tokens[cmd_idx].lower()
        auto_release = False
        arg = 1.0

        if len(tokens) > cmd_idx + 1:
            try:
                arg = float(tokens[cmd_idx + 1])
                arg = max(-1.0, min(1.0, arg))
            except ValueError:
                arg = 1.0
        else:
            arg = 1.0
            auto_release = True

        # Digital active state (1 for active, 0 for inactive)
        is_active = (abs(arg) >= 0.5)

        # Process button commands
        if cmd == "a":
            self.set_button(BTN_A, is_active)
        elif cmd == "b":
            self.set_button(BTN_B, is_active)
        elif cmd == "x":
            self.set_button(BTN_X, is_active)
        elif cmd == "y":
            self.set_button(BTN_Y, is_active)
        elif cmd == "h":
            self.set_button(BTN_HOME, is_active)
        elif cmd == "c":
            self.set_button(BTN_CAPTURE, is_active)
        elif cmd in ("m", "-"):
            self.set_button(BTN_MINUS, is_active)
        elif cmd in ("p", "+"):
            self.set_button(BTN_PLUS, is_active)
        elif cmd == "l1":
            self.set_button(BTN_L, is_active)
        elif cmd == "l2":
            self.set_button(BTN_ZL, is_active)
        elif cmd == "r1":
            self.set_button(BTN_R, is_active)
        elif cmd == "r2":
            self.set_button(BTN_ZR, is_active)
        elif cmd == "lp":
            self.set_button(BTN_LSTICK, is_active)
        elif cmd == "rp":
            self.set_button(BTN_RSTICK, is_active)

        # Process D-Pad commands
        elif cmd == "pu":
            self.dpad_up = is_active
        elif cmd == "pd":
            self.dpad_down = is_active
        elif cmd == "pl":
            self.dpad_left = is_active
        elif cmd == "pr":
            self.dpad_right = is_active
        elif cmd == "pur":
            self.dpad_up = is_active
            self.dpad_right = is_active
        elif cmd == "pul":
            self.dpad_up = is_active
            self.dpad_left = is_active
        elif cmd == "pdr":
            self.dpad_down = is_active
            self.dpad_right = is_active
        elif cmd == "pdl":
            self.dpad_down = is_active
            self.dpad_left = is_active
        elif cmd == "px":
            if arg >= 0.5:
                self.dpad_right = True
                self.dpad_left = False
            elif arg <= -0.5:
                self.dpad_left = True
                self.dpad_right = False
            else:
                self.dpad_left = False
                self.dpad_right = False
        elif cmd == "py":
            if arg >= 0.5:
                self.dpad_up = True
                self.dpad_down = False
            elif arg <= -0.5:
                self.dpad_down = True
                self.dpad_up = False
            else:
                self.dpad_up = False
                self.dpad_down = False

        # Process Left Stick commands
        elif cmd == "lx":
            self.lx = arg
        elif cmd == "ly":
            self.ly = arg
        elif cmd == "lu":
            self.lx = 0.0
            self.ly = -1.0 if is_active else 0.0
        elif cmd == "ld":
            self.lx = 0.0
            self.ly = 1.0 if is_active else 0.0
        elif cmd == "ll":
            self.lx = -1.0 if is_active else 0.0
            self.ly = 0.0
        elif cmd == "lr":
            self.lx = 1.0 if is_active else 0.0
            self.ly = 0.0
        elif cmd == "lur":
            self.lx = 1.0 if is_active else 0.0
            self.ly = -1.0 if is_active else 0.0
        elif cmd == "lul":
            self.lx = -1.0 if is_active else 0.0
            self.ly = -1.0 if is_active else 0.0
        elif cmd == "ldr":
            self.lx = 1.0 if is_active else 0.0
            self.ly = 1.0 if is_active else 0.0
        elif cmd == "ldl":
            self.lx = -1.0 if is_active else 0.0
            self.ly = 1.0 if is_active else 0.0

        # Process Right Stick commands
        elif cmd == "rx":
            self.rx = arg
        elif cmd == "ry":
            self.ry = arg
        elif cmd == "ru":
            self.rx = 0.0
            self.ry = -1.0 if is_active else 0.0
        elif cmd == "rd":
            self.rx = 0.0
            self.ry = 1.0 if is_active else 0.0
        elif cmd == "rl":
            self.rx = -1.0 if is_active else 0.0
            self.ry = 0.0
        elif cmd == "rr":
            self.rx = 1.0 if is_active else 0.0
            self.ry = 0.0
        elif cmd == "rur":
            self.rx = 1.0 if is_active else 0.0
            self.ry = -1.0 if is_active else 0.0
        elif cmd == "rul":
            self.rx = -1.0 if is_active else 0.0
            self.ry = -1.0 if is_active else 0.0
        elif cmd == "rdr":
            self.rx = 1.0 if is_active else 0.0
            self.ry = 1.0 if is_active else 0.0
        elif cmd == "rdl":
            self.rx = -1.0 if is_active else 0.0
            self.ry = 1.0 if is_active else 0.0
        else:
            return

        self.sync_report()

        # Handle scheduled auto-release
        now = time.monotonic()
        if auto_release:
            release_delay = duration_s if duration_s > 0.0 else DEFAULT_AUTO_RELEASE_SECONDS
            self.scheduled_commands.append((now + release_delay, f"{cmd} 0"))

    def check_scheduled(self) -> None:
        # Executes scheduled auto-releases that reached their expiration timestamp
        if not self.scheduled_commands:
            return

        now = time.monotonic()
        remaining: list[tuple[float, str]] = []

        for target_time, cmd in self.scheduled_commands:
            if now >= target_time:
                self.execute_command(cmd)
            else:
                remaining.append((target_time, cmd))

        self.scheduled_commands = remaining


# Main execution entry point
def main() -> None:
    print("Starting Nintendo Switch Controller TCP Backend (nsbackend-pico)...")

    # Load configuration
    config = load_toml_config()
    hostname: str = str(config.get("hostname", "nscon"))
    tcp_port: int = int(config.get("tcp_port", 10100))
    log_enabled: bool = bool(config.get("log", False))
    led_active_low: bool = bool(config.get("led_active_low", False))
    enable_echo: bool = bool(config.get("enable_echo", False))

    # Initialize status LED indicator and turn ON during initialization
    led = StatusLed(active_low=led_active_low)
    led.set_step(LedState.INITIALIZING)

    wifi_manager = WifiManager(config)
    print(f"Loaded config: hostname='{hostname}', wifi_ssids={wifi_manager.configured_ssids}, tcp_port={tcp_port}, log={log_enabled}, led_active_low={led_active_low}, enable_echo={enable_echo}")

    # Connect to Wi-Fi
    wifi_manager.connect(led)

    # Initialize mDNS hostname advertisement
    print(f"Advertising mDNS hostname: {hostname}.local...")
    mdns_server = mdns.Server(wifi.radio)
    mdns_server.hostname = hostname
    mdns_server.advertise_service(service_type="_custom", protocol="_tcp", port=tcp_port)
    print(f"mDNS active: {hostname}.local -> {wifi.radio.ipv4_address}")

    # Initialize Switch Gamepad HID interface
    print(f"Initializing the USB HID Switch Gamepad interface...")
    gamepad = SwitchGamepad()
    controller = ControllerState(gamepad)
    controller.reset_all()

    # Setup TCP Server socket
    print(f"Starting TCP sever...")
    pool = socketpool.SocketPool(wifi.radio)
    server_socket = pool.socket(pool.AF_INET, pool.SOCK_STREAM)
    server_socket.bind(("0.0.0.0", tcp_port))
    server_socket.listen(1)
    server_socket.settimeout(0.05)

    print(f"TCP server listening on {wifi.radio.ipv4_address}:{tcp_port} ({hostname}.local:{tcp_port})")

    # Transition to waiting for client
    led.set_step(LedState.WAITING_CLIENT)

    rx_buffer = bytearray(512)

    while True:
        led.update()

        # Check and maintain Wi-Fi connection
        if not wifi.radio.connected:
            print("Wi-Fi disconnected. Reconnecting...")
            led.set_step(LedState.INITIALIZING)
            wifi_manager.connect(led)
            led.set_step(LedState.WAITING_CLIENT)

        controller.check_scheduled()

        try:
            client_socket, client_address = server_socket.accept()
        except OSError:
            # Socket accept timeout, continue polling
            continue

        print(f"Client connected from {client_address}")
        # Client connected
        led.set_step(LedState.CLIENT_CONNECTED)

        # Short timeout on client socket for continuous non-blocking stream processing
        client_socket.settimeout(0.02)
        stream_accum = ""

        try:
            while True:
                led.update()
                controller.check_scheduled()

                try:
                    bytes_received = client_socket.recv_into(rx_buffer)
                    if bytes_received == 0:
                        print(f"Client {client_address} disconnected (EOF).")
                        break

                    chunk = rx_buffer[:bytes_received].decode("utf-8", "ignore")
                    stream_accum += chunk

                    while "\n" in stream_accum:
                        line, stream_accum = stream_accum.split("\n", 1)
                        line = line.strip()
                        if line:
                            if log_enabled:
                                print(line)
                            if enable_echo:
                                try:
                                    client_socket.send((line + "\n").encode("utf-8"))
                                except OSError:
                                    pass
                            controller.execute_command(line)

                except OSError as e:
                    err_msg = str(e)
                    if "ETIMEDOUT" in err_msg or "EAGAIN" in err_msg or "timed out" in err_msg or (e.args and e.args[0] in (11, 110)):
                        pass
                    else:
                        print(f"Client {client_address} connection error: {e}")
                        break

        except Exception as e:
            print(f"Exception handling client {client_address}: {e}")
        finally:
            print(f"Closing client session {client_address} and resetting controller.")
            try:
                client_socket.close()
            except Exception:
                pass
            controller.reset_all()
            # Transition back to waiting for client
            led.set_step(LedState.WAITING_CLIENT)


if __name__ == "__main__":
    main()
