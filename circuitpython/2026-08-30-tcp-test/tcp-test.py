#!/usr/bin/env circuit-run
#file: config.toml
#
# TCP Test Server with mDNS advertisement for CircuitPython (Raspberry Pi Pico 2 W / ESP32).
#
# Reads Wi-Fi credentials, hostname, and TCP port from config.toml.
# Connects to the configured Wi-Fi AP, advertises its hostname via mDNS (e.g., tcp-test.local),
# and runs a TCP server on the configured port that logs all incoming data to the serial console.

import time
import mdns
import socketpool
import wifi

# Configuration file location on the CircuitPython filesystem
CONFIG_FILE_PATH: str = "config.toml"


# Robust key-value parser for simple TOML files in CircuitPython
def load_toml_config(file_path: str) -> dict[str, str | int]:
    # Parse key = "value" or key = 123 from a TOML configuration file
    config: dict[str, str | int] = {}
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
    return config


# Wi-Fi connection handler
def connect_wifi(ssid: str, password: str) -> None:
    # Connect to the Wi-Fi network with retry loop
    if wifi.radio.connected:
        print(f"Already connected to Wi-Fi. IP: {wifi.radio.ipv4_address}")
        return

    print(f"Connecting to Wi-Fi SSID: '{ssid}'...")
    while not wifi.radio.connected:
        try:
            wifi.radio.connect(ssid, password)
            print(f"Connected to Wi-Fi successfully! Assigned IP: {wifi.radio.ipv4_address}")
        except Exception as e:
            print(f"Wi-Fi connection failed: {e}. Retrying in 3 seconds...")
            time.sleep(3.0)


# Main TCP server execution loop
def main() -> None:
    # Load configuration
    print(f"Loading configuration from '{CONFIG_FILE_PATH}'...")
    config = load_toml_config(CONFIG_FILE_PATH)

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
