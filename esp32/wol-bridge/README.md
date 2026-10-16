# ESP32 Wake-on-LAN (WoL) Bridge

A dedicated Wake-on-LAN hardware bridge for **ESP32-S3** and the **WIZnet W5500 (USR-ES1)** SPI Ethernet module.

It connects **directly to a PC's Ethernet port** (no hub or switch needed), monitors Layer 2 Ethernet traffic in promiscuous mode to automatically learn all active MAC addresses (including virtual machines), persists them across power cycles in non-volatile storage, and exposes an HTTP endpoint (`http://wol.local/wake`) over Wi-Fi to wake the PC on demand.

---

## Features

- **Direct PC Connection**: Connects straight into the PC's Ethernet jack via a standard patch cable (Gigabit NIC Auto-MDIX handles polarity automatically).
- **Automatic MAC Discovery**: Operates the W5500 in promiscuous mode (`ETH_CMD_S_PROMISCUOUS`) to sniff incoming raw Layer-2 Ethernet frames (DHCP discovers, ARP queries, IPv6 RS, mDNS) and records all source MAC addresses without requiring a DHCP server on the cable.
- **VM Support**: Tracks multiple MAC addresses simultaneously to wake hosts with bridged virtual machines.
- **Smart Purge Strategy**:
  - Automatically persists learned MAC addresses to NVS flash so they survive ESP32 power interruptions while the PC is off.
  - When a Wake-on-LAN packet is triggered via Wi-Fi, the bridge arms an automatic purge: once the PC wakes up and transmits its first packet, all stale MAC addresses from previous PCs are purged, and tracking restarts fresh with that new machine.
  - Manual purge is also available at `POST /purge`.
- **Dual-Protocol Wake-on-LAN Transmission**: Transmits both raw Layer 2 Magic Packets (`EtherType 0x0842`) and UDP port 9 broadcast magic packets to all recorded and static MACs.
- **Wi-Fi & mDNS**: Connects to 2.4 GHz Wi-Fi with multi-AP fallback and advertises as `http://wol.local/` via mDNS.
- **Embedded Web UI & API**:
  - `GET /`: Responsive web dashboard showing link status, purge state, Wi-Fi IP, and recorded MAC table.
  - `POST /wake` or `GET /wake`: Sends WoL packets to all recorded MAC addresses.
  - `GET /status`: Returns JSON device status and MAC table.
  - `POST /purge`: Clears all learned MAC addresses.
- **TinyUSB MSC Drag-and-Drop Configuration**: Exposes internal FATFS storage partition as a USB flash drive to edit `config.toml` directly on your PC.

---

## Hardware Pinout Documentation

### Universal Pinout Compatibility Table

| Signal | ESP32-S3 GPIO | DevKitC-1 Physical Pin | Seeed Studio XIAO Pin | W5500 Module Pin | Details |
| :--- | :---: | :---: | :---: | :---: | :--- |
| **SPI SCLK** | `GPIO7` | Row B, Pin 7 (silk `7`) | `D8` (Pin 9) | `SCK` | SPI Clock (25 MHz default) |
| **SPI MOSI** | `GPIO9` | Row B, Pin 15 (silk `9`) | `D10` (Pin 11) | `MOSI` | Master Out Slave In |
| **SPI MISO** | `GPIO8` | Row B, Pin 12 (silk `8`) | `D9` (Pin 10) | `MISO` | Master In Slave Out |
| **SPI CS** | `GPIO4` | Row B, Pin 4 (silk `4`) | `D3` (Pin 4) | `CS` | Active-low Chip Select |
| **Hardware Reset** | `GPIO3` | Row B, Pin 13 (silk `3`) | `D2` (Pin 3) | `RST` | 160 ms hardware reset sequencing |
| **Hardware INT** | `GPIO2` | Row A, Pin 5 (silk `2`) | `D1` (Pin 2) | `INT` | Active-low hardware interrupt |
| **Status LED** | `GPIO21` | Row A, Pin 18 (silk `21`) | Onboard Yellow LED | — | Active-low status indicator |
| **3.3V Power** | `3V3` | Row B, Pin 1 or 2 | `3V3` (Pin 12) | `3V3` | 3.3V DC power rail |
| **Ground** | `GND` | Row B, Pin 22 | `GND` (Pin 13) | `G` | Common digital ground |

---

## Workflow Scripts

| Script | Purpose | Description |
| :--- | :--- | :--- |
| [`00-build.sh`](00-build.sh) | **Build Firmware** | Activates ESP-IDF environment, builds firmware binary and FATFS storage image. |
| [`01-install.sh`](01-install.sh) | **Flash Board** | Flashes bootloader, partition table, firmware, and FATFS image. Auto-detects serial port. |
| [`02-monitor.sh`](02-monitor.sh) | **Monitor Serial** | Symlink to `tools/monitor.sh`. Connects to UART or TinyUSB CDC serial console. |

---

## Configuration (`config.toml`)

Edit `fatfs_data/config.toml` (or export `$ESP32_CONFIG_TOML` to override secrets without touching git):

```toml
# wol-bridge Configuration
hostname = "wol"
http_port = 80
led_pin = 21
led_active_low = true

# Primary Wi-Fi Access Point
wifi_ssid = "YOUR_WIFI_SSID"
wifi_password = "YOUR_WIFI_PASSWORD"

# Static target MAC addresses (optional, comma-separated)
# static_macs = "AA:BB:CC:DD:EE:FF"

# W5500 SPI Pin Configuration
spi_sck   = 7
spi_mosi  = 9
spi_miso  = 8
spi_cs    = 4
spi_reset = 3
spi_int   = 2
poll_period_ms = 2
spi_speed_mhz  = 25
```

---

## HTTP API Usage

- **Wake the PC**:
  ```bash
  curl http://wol.local/wake
  # Or request JSON response:
  curl -H "Accept: application/json" http://wol.local/wake
  ```

- **Query status and recorded MACs**:
  ```bash
  curl http://wol.local/status
  ```

- **Purge learned MACs**:
  ```bash
  curl -X POST http://wol.local/purge
  ```
