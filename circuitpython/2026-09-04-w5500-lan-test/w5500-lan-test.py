#!/usr/bin/env circuit-run
#file: config.toml
#file: ../libs/adafruit_wiznet5k
#file: ../libs/adafruit_ticks.py
#
# Sample TCP Echo Server with DHCP for Raspberry Pi Pico and USR-ES1 (WIZnet W5500) Ethernet module.
#
# Reads hostname and TCP port from config.toml (overridden by config-base.toml if present).
# Connects to the W5500 SPI Ethernet module using recommended SPI0 pin mappings,
# acquires an IP address via DHCP, and runs a TCP echo server on port 10110.
#
# Note on mDNS:
# CircuitPython's built-in `mdns` module is strictly bound to native Wi-Fi (`wifi.radio`)
# in the core C firmware and cannot bind to SPI-based Ethernet controllers. Network clients
# connect directly to the assigned DHCP IP address printed to the serial console.

import errno
import time
import board
import busio
import digitalio
import microcontroller
from adafruit_wiznet5k.adafruit_wiznet5k import WIZNET5K
import adafruit_wiznet5k.adafruit_wiznet5k_socketpool as socketpool

# Default hardware SPI0 pin assignments matching the recommended USR-ES1 wiring:
# - USR-ES1 J1-4 (SCLK) -> Pico GP18 (Pin 24)
# - USR-ES1 J1-3 (MOSI) -> Pico GP19 (Pin 25)
# - USR-ES1 J2-6 (MISO) -> Pico GP16 (Pin 21)
# - USR-ES1 J1-5 (SCSn) -> Pico GP17 (Pin 22)
# - USR-ES1 J2-5 (RSTn) -> Pico GP20 (Pin 26)
DEFAULT_PIN_SPI_SCK: int = 18
DEFAULT_PIN_SPI_MOSI: int = 19
DEFAULT_PIN_SPI_MISO: int = 16
DEFAULT_PIN_SPI_CS: int = 17
DEFAULT_PIN_SPI_RESET: int = 20

# Disconnect and timeout error codes safely resolved across platforms and MicroPython/CircuitPython
DISCONNECT_ERRNOS: set[int] = {
    getattr(errno, name)
    for name in ("ECONNRESET", "ENOTCONN", "ESHUTDOWN", "ECONNABORTED")
    if hasattr(errno, name)
}
TIMEOUT_ERRNOS: set[int] = {
    getattr(errno, name)
    for name in ("ETIMEDOUT", "EAGAIN", "EWOULDBLOCK")
    if hasattr(errno, name)
}

# Configuration file paths on the CircuitPython filesystem
CONFIG_FILE_PATH: str = "config.toml"
CONFIG_BASE_FILE_PATH: str = "config-base.toml"


# Parse simple key-value pairs from a TOML configuration file into a dictionary
def parse_toml_file(file_path: str, config: dict[str, str | int | bool]) -> bool:
    try:
        with open(file_path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                # Skip blank lines and full-line comments
                if not line or line.startswith("#"):
                    continue
                if "=" in line:
                    key_part, val_part = line.split("=", 1)
                    key = key_part.strip()
                    val = val_part.strip()

                    # Strip trailing comments if value is not quoted
                    if "#" in val and not ((val.startswith('"') and val.endswith('"')) or (val.startswith("'") and val.endswith("'"))):
                        val = val.split("#", 1)[0].strip()

                    # Parse quoted string values
                    if (val.startswith('"') and val.endswith('"')) or (val.startswith("'") and val.endswith("'")):
                        config[key] = val[1:-1]
                    # Parse numeric integer values
                    elif val.isdigit() or (val.startswith("-") and val[1:].isdigit()):
                        config[key] = int(val)
                    # Parse boolean values
                    elif val.lower() == "true":
                        config[key] = True
                    elif val.lower() == "false":
                        config[key] = False
                    else:
                        config[key] = val
        return True
    except OSError:
        return False


# Load TOML configuration with optional base file override
def load_toml_config(file_path: str = CONFIG_FILE_PATH, base_path: str = CONFIG_BASE_FILE_PATH) -> dict[str, str | int | bool]:
    config: dict[str, str | int | bool] = {}
    if parse_toml_file(file_path, config):
        print(f"Loaded config from '{file_path}'")
    if parse_toml_file(base_path, config):
        print(f"Loaded override config from '{base_path}'")
    return config


# Perform a clean hardware reset on the W5500 chip
def reset_w5500_hardware(rst_pin: digitalio.DigitalInOut) -> None:
    # The W5500 datasheet requires asserting RSTn LOW for at least 2 microseconds,
    # followed by waiting at least 150 ms after deasserting HIGH for the internal PLL to stabilize.
    rst_pin.switch_to_output(value=True)
    time.sleep(0.01)
    rst_pin.value = False
    time.sleep(0.005)  # 5 ms LOW pulse
    rst_pin.value = True
    time.sleep(0.160)  # 160 ms stabilization delay


# Format hardware MAC address bytes as a standard colon-separated hex string
def format_mac_address(mac_bytes: bytes | list[int] | tuple[int, ...]) -> str:
    return ":".join(f"{b:02X}" for b in mac_bytes)


# Resolve a GPIO pin identifier on the current board
def get_pin(pin_id: int | str) -> board.Pin:
    if isinstance(pin_id, str):
        if hasattr(board, pin_id):
            return getattr(board, pin_id)
        digits = "".join([c for c in pin_id if c.isdigit()])
        num = int(digits) if digits else 0
    else:
        num = int(pin_id)
    for candidate in (f"GP{num}", f"IO{num}", f"D{num}", f"GPIO{num}"):
        if hasattr(board, candidate):
            return getattr(board, candidate)
    raise ValueError(f"GPIO pin {pin_id} not found on this board")


# Obtain hardware MAC address from configuration, or default to standard test MAC
def get_mac_address(config: dict[str, Any]) -> tuple[int, ...] | str:
    mac_cfg = config.get("mac")
    if mac_cfg:
        return str(mac_cfg)
    return "DE:AD:BE:EF:FE:ED"


# Main TCP server application routine
def main() -> None:
    # Read runtime configuration
    config = load_toml_config()

    hostname: str = str(config.get("hostname", "w5500-test"))
    tcp_port: int = int(config.get("tcp_port", 10110))

    pin_sck_id: int = int(config.get("spi_sck", DEFAULT_PIN_SPI_SCK))
    pin_mosi_id: int = int(config.get("spi_mosi", DEFAULT_PIN_SPI_MOSI))
    pin_miso_id: int = int(config.get("spi_miso", DEFAULT_PIN_SPI_MISO))
    pin_cs_id: int = int(config.get("spi_cs", DEFAULT_PIN_SPI_CS))
    pin_reset_id: int = int(config.get("spi_reset", DEFAULT_PIN_SPI_RESET))

    print(f"Configuration: hostname='{hostname}', tcp_port={tcp_port}")
    print(f"SPI Pins: SCK=GP{pin_sck_id}, MOSI=GP{pin_mosi_id}, MISO=GP{pin_miso_id}, CS=GP{pin_cs_id}, RST=GP{pin_reset_id}")

    # Resolve board pins
    pin_sck = get_pin(pin_sck_id)
    pin_mosi = get_pin(pin_mosi_id)
    pin_miso = get_pin(pin_miso_id)
    pin_cs_obj = get_pin(pin_cs_id)
    pin_reset_obj = get_pin(pin_reset_id)

    # Initialize Reset and Chip Select GPIO pins
    rst = digitalio.DigitalInOut(pin_reset_obj)
    cs = digitalio.DigitalInOut(pin_cs_obj)
    cs.switch_to_output(value=True)

    print("Executing hardware reset on W5500...")
    reset_w5500_hardware(rst)

    # Initialize SPI bus at 20 MHz (W5500 supports up to 80 MHz)
    spi = busio.SPI(clock=pin_sck, MOSI=pin_mosi, MISO=pin_miso)

    mac_addr = get_mac_address(config)
    print("Initializing WIZNET5K driver...")
    eth = WIZNET5K(spi, cs, is_dhcp=False, mac=mac_addr, hostname=hostname)
    # Configure 400 ms retry interval and 10 retries for robust ARP resolution across Wi-Fi bridges
    eth.rtr = 4000
    eth.rcr = 10

    # Verify physical Ethernet link
    print("Checking physical Ethernet link state...")
    while not eth.link_status:
        print("Waiting for Ethernet cable connection (link is DOWN)...")
        time.sleep(1.0)

    print("Ethernet cable connected (link is UP)! Requesting DHCP lease...")
    while True:
        try:
            eth.set_dhcp(hostname=hostname)
            ip_str = eth.pretty_ip(eth.ip_address)
            if ip_str != "0.0.0.0":
                break
        except Exception as e:
            print(f"Waiting for DHCP lease: {e}")
            time.sleep(1.0)

    ip_addr, subnet_mask, gateway, dns_server = eth.ifconfig
    print("-" * 48)
    print(f"Ethernet Connected!")
    print(f"  MAC Address:  {format_mac_address(eth.mac_address)}")
    print(f"  IP Address:   {eth.pretty_ip(ip_addr)}")
    print(f"  Subnet Mask:  {eth.pretty_ip(subnet_mask)}")
    print(f"  Gateway:      {eth.pretty_ip(gateway)}")
    print(f"  DNS Server:   {eth.pretty_ip(dns_server)}")
    print(f"  Hostname:     {hostname}")
    print("-" * 48)

    # Setup TCP Server Socket
    pool = socketpool.SocketPool(eth)

    # Transmit a broadcast packet to populate switch and bridge forwarding tables immediately
    try:
        announcer_sock = pool.socket(pool.AF_INET, pool.SOCK_DGRAM)
        announcer_sock.sendto(b"\x00", ("255.255.255.255", 9))
        announcer_sock.close()
    except Exception as announce_err:
        print(f"Network announcement notification: {announce_err}")

    server_socket = pool.socket(pool.AF_INET, pool.SOCK_STREAM)
    server_socket.bind((eth.pretty_ip(eth.ip_address), tcp_port))
    server_socket.listen(1)
    # Set non-infinite timeout to allow polling link state
    server_socket.settimeout(1.0)

    print(f"TCP Echo Server listening on {eth.pretty_ip(eth.ip_address)}:{tcp_port}")
    print(f"Connect via: nc {eth.pretty_ip(eth.ip_address)} {tcp_port}")

    rx_buffer = bytearray(1024)
    last_announce_time = time.monotonic()

    while True:
        # Monitor Ethernet physical link
        if not eth.link_status:
            print("Warning: Ethernet link lost. Waiting for cable reconnection...")
            while not eth.link_status:
                time.sleep(1.0)
            print(f"Ethernet cable reconnected. IP: {eth.pretty_ip(eth.ip_address)}")

        # Accept incoming client connection
        try:
            client_socket, client_address = server_socket.accept()
        except (TimeoutError, OSError):
            now = time.monotonic()
            if now - last_announce_time > 3.0:
                last_announce_time = now
                try:
                    probe_sock = pool.socket(pool.AF_INET, pool.SOCK_DGRAM)
                    probe_sock.sendto(b"\x00", ("255.255.255.255", 9))
                    probe_sock.close()
                except Exception:
                    pass
            time.sleep(0.01)
            continue

        print(f"Client connected from {client_address}")
        # Keep client socket in blocking mode to receive arbitrary multi-line payloads
        client_socket.settimeout(None)

        try:
            while True:
                try:
                    bytes_received = client_socket.recv_into(rx_buffer)
                except (TimeoutError, OSError) as sock_err:
                    err_code = getattr(sock_err, "errno", None)
                    if err_code in DISCONNECT_ERRNOS:
                        print(f"Client {client_address} closed connection.")
                        break
                    if isinstance(sock_err, TimeoutError) or err_code in TIMEOUT_ERRNOS:
                        # Normal socket read timeout, yield briefly to avoid tight polling
                        time.sleep(0.01)
                        continue
                    print(f"Socket receive error from {client_address}: {sock_err}")
                    break

                if bytes_received == 0:
                    print(f"Client {client_address} disconnected.")
                    break

                data_chunk = bytes(rx_buffer[:bytes_received])
                try:
                    decoded_text = data_chunk.decode("utf-8")
                    print(
                        f"[{client_address}] Received ({bytes_received} bytes): {decoded_text}",
                        end="" if decoded_text.endswith("\n") else "\n",
                    )
                except Exception:
                    print(f"[{client_address}] Received raw bytes ({bytes_received} bytes): {data_chunk!r}")

                # Echo the received data back to client
                print(f"[{client_address}] Echoing {len(data_chunk)} bytes...")
                try:
                    sent = client_socket.send(data_chunk)
                    print(f"[{client_address}] Echoed {sent} bytes.")
                except Exception as send_err:
                    print(f"[{client_address}] Error echoing data: {send_err}")
                    break
        except Exception as client_err:
            print(f"Error handling client {client_address}: {client_err}")
        finally:
            client_socket.close()


if __name__ == "__main__":
    main()
