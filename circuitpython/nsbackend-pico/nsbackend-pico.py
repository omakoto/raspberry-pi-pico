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

# Configuration file location on the CircuitPython filesystem
CONFIG_FILE_PATH: str = "config.toml"

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
                self.off()
                print(f"Status LED initialized on board.LED (active_low={self.active_low})")
            except Exception as e:
                print(f"Warning: Failed to initialize digitalio on board.LED: {e}")
        else:
            print("Warning: board.LED is not defined on this board")

        self.current_step: int = LedState.INITIALIZING
        self.step_start_time: float = time.monotonic()

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


# Load basic TOML configuration file
def load_toml_config(file_path: str) -> dict[str, str | int]:
    # Parse key = "value" or key = 123 from configuration file
    config: dict[str, str | int] = {}
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
    return config


# Connect to Wi-Fi network
def connect_wifi(ssid: str, password: str, led: StatusLed) -> None:
    if wifi.radio.connected:
        print(f"Already connected to Wi-Fi. IP: {wifi.radio.ipv4_address}")
        return

    led.set_step(LedState.INITIALIZING)
    print(f"Connecting to Wi-Fi SSID: '{ssid}'...")
    while not wifi.radio.connected:
        try:
            wifi.radio.connect(ssid, password)
            print(f"Connected to Wi-Fi successfully! IP: {wifi.radio.ipv4_address}")
            return
        except Exception as e:
            print(f"Wi-Fi connection failed: {e}. Retrying in 3 seconds...")
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
            # Invert stick Y matching nsbackend convention
            self.ly = -arg
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
            self.ry = -arg
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
    config = load_toml_config(CONFIG_FILE_PATH)
    hostname: str = str(config.get("hostname", "nscon"))
    wifi_ssid: str = str(config.get("wifi_ssid", ""))
    wifi_password: str = str(config.get("wifi_password", ""))
    tcp_port: int = int(config.get("tcp_port", 10100))
    log_enabled: bool = bool(config.get("log", False))
    led_active_low: bool = bool(config.get("led_active_low", False))

    # Initialize status LED indicator and turn ON during initialization
    led = StatusLed(active_low=led_active_low)
    led.set_step(LedState.INITIALIZING)

    print(f"Loaded config: hostname='{hostname}', wifi_ssid='{wifi_ssid}', tcp_port={tcp_port}, log={log_enabled}, led_active_low={led_active_low}")

    # Connect to Wi-Fi
    connect_wifi(wifi_ssid, wifi_password, led)

    # Initialize mDNS hostname advertisement
    print(f"Advertising mDNS hostname: {hostname}.local...")
    mdns_server = mdns.Server(wifi.radio)
    mdns_server.hostname = hostname
    mdns_server.advertise_service(service_type="_custom", protocol="_tcp", port=tcp_port)
    print(f"mDNS active: {hostname}.local -> {wifi.radio.ipv4_address}")

    # Initialize Switch Gamepad HID interface
    gamepad = SwitchGamepad()
    controller = ControllerState(gamepad)
    controller.reset_all()

    # Setup TCP Server socket
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
            connect_wifi(wifi_ssid, wifi_password, led)
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
