#!/usr/bin/env circuit-run
#file: config.toml
#
# TCP Test Server with mDNS advertisement for CircuitPython (Raspberry Pi Pico 2 W / ESP32).
#
# Reads Wi-Fi credentials, hostname, and TCP port from config.toml (overridden by config-base.toml if present).
# Connects to the configured Wi-Fi AP, advertises its hostname via mDNS (e.g., tcp-test.local),
# and runs a TCP server on the configured port that logs all incoming data to the serial console.

import time
import mdns
import socketpool
import wifi

# Configuration file locations on the CircuitPython filesystem
CONFIG_FILE_PATH: str = "config.toml"
CONFIG_BASE_FILE_PATH: str = "config-base.toml"


# Parse simple key-value pairs from a TOML file into the config dictionary
def parse_toml_file(file_path: str, config: dict[str, str | int | bool]) -> bool:
    try:
        with open(file_path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                # Skip blank lines and whole-line comments
                if not line or line.startswith("#"):
                    continue
                if "=" in line:
                    key_part, val_part = line.split("=", 1)
                    key = key_part.strip()
                    val = val_part.strip()

                    # Remove trailing comments if not enclosed in quotes
                    if "#" in val and not ((val.startswith('"') and val.endswith('"')) or (val.startswith("'") and val.endswith("'"))):
                        val = val.split("#", 1)[0].strip()

                    # Parse quoted string
                    if (val.startswith('"') and val.endswith('"')) or (val.startswith("'") and val.endswith("'")):
                        config[key] = val[1:-1]
                    # Parse integer
                    elif val.isdigit() or (val.startswith("-") and val[1:].isdigit()):
                        config[key] = int(val)
                    # Parse boolean
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


# Scans visible Wi-Fi access points and prints diagnostic details
def scan_and_print_networks() -> None:
    print("Scanning visible Wi-Fi networks...")
    try:
        count = 0
        for net in wifi.radio.start_scanning_networks():
            ssid_name = net.ssid if net.ssid else "<hidden>"
            print(f"  [AP] SSID: '{ssid_name}', RSSI: {net.rssi} dBm, Ch: {net.channel}")
            count += 1
        wifi.radio.stop_scanning_networks()
        if count == 0:
            print("  No visible Wi-Fi networks found.")
    except Exception as e:
        print(f"  Wi-Fi scan failed: {e}")


# Wi-Fi connection handler
def connect_wifi(ssid: str, password: str) -> None:
    # Connect to the Wi-Fi network with retry loop
    if wifi.radio.connected:
        print(f"Already connected to Wi-Fi. IP: {wifi.radio.ipv4_address}")
        return

    print(f"Connecting to Wi-Fi SSID: '{ssid}' (password length: {len(password)})...")
    while not wifi.radio.connected:
        try:
            wifi.radio.connect(ssid, password)
            print(f"Connected to Wi-Fi successfully! Assigned IP: {wifi.radio.ipv4_address}")
            return
        except Exception as e:
            err_msg = format_wifi_error(e)
            print(f"Wi-Fi connection failed: {err_msg}")
            scan_and_print_networks()


# Main TCP server execution loop
def main() -> None:
    # Load configuration
    config = load_toml_config()

    hostname: str = str(config.get("hostname", "tcp-test"))
    wifi_ssid: str = str(config.get("wifi_ssid", ""))
    wifi_password: str = str(config.get("wifi_password", ""))
    tcp_port: int = int(config.get("tcp_port", 10100))

    print(f"Loaded config: hostname='{hostname}', wifi_ssid='{wifi_ssid}', tcp_port={tcp_port}")

    # Connect to Wi-Fi
    connect_wifi(wifi_ssid, wifi_password)

    # Advertise IP address using mDNS
    print(f"Starting mDNS server advertising as '{hostname}.local'...")
    mdns_server = mdns.Server(wifi.radio)
    mdns_server.hostname = hostname
    mdns_server.advertise_service(service_type="_custom", protocol="_tcp", port=tcp_port)
    print(f"mDNS active: {hostname}.local -> {wifi.radio.ipv4_address}")

    # Set up TCP server socket
    pool = socketpool.SocketPool(wifi.radio)
    server_socket = pool.socket(pool.AF_INET, pool.SOCK_STREAM)
    server_socket.bind(("0.0.0.0", tcp_port))
    server_socket.listen(1)
    # Set timeout for socket accept so main loop can monitor Wi-Fi status
    server_socket.settimeout(1.0)

    print(f"TCP server listening on {wifi.radio.ipv4_address}:{tcp_port} ({hostname}.local:{tcp_port})")

    rx_buffer = bytearray(1024)

    while True:
        # Verify Wi-Fi connection is still active
        if not wifi.radio.connected:
            print("Wi-Fi disconnected. Reconnecting...")
            connect_wifi(wifi_ssid, wifi_password)

        try:
            client_socket, client_address = server_socket.accept()
        except OSError:
            # Timeout waiting for a connection, continue polling
            continue

        print(f"Client connected from: {client_address}")
        # Set timeout on client socket for receiving data
        client_socket.settimeout(0.5)

        try:
            while True:
                try:
                    bytes_received = client_socket.recv_into(rx_buffer)
                    if bytes_received == 0:
                        # Client disconnected
                        print(f"Client {client_address} disconnected.")
                        break

                    received_data = bytes(rx_buffer[:bytes_received])
                    try:
                        decoded_text = received_data.decode("utf-8")
                        print(f"[{client_address}] Received ({bytes_received} bytes): {decoded_text}", end="" if decoded_text.endswith("\n") else "\n")
                    except Exception:
                        print(f"[{client_address}] Received raw bytes ({bytes_received} bytes): {received_data!r}")
                except OSError:
                    # Read timeout; loop to continue listening or check connection
                    continue
        except Exception as e:
            print(f"Error handling client {client_address}: {e}")
        finally:
            client_socket.close()


if __name__ == "__main__":
    main()
