# ESP32 Wake-on-LAN (WoL) Bridge

A dedicated Wake-on-LAN hardware bridge for **ESP32-S3** and the **WIZnet W5500 (USR-ES1)** SPI Ethernet module.

It connects **directly to a PC's Ethernet port** (no hub or switch needed), monitors Layer 2 Ethernet traffic in promiscuous mode to automatically learn all active MAC addresses (including virtual machines), persists them across power cycles in non-volatile storage, and exposes an HTTP endpoint (`http://wol.local/wake`) over Wi-Fi to wake the PC on demand.

---

## Features

- **Direct PC Connection**: Connects straight into the PC's Ethernet jack via a standard patch cable (Gigabit NIC Auto-MDIX handles polarity automatically).
- **Automatic MAC Discovery**: Operates the W5500 in promiscuous mode (`ETH_CMD_S_PROMISCUOUS`) to sniff incoming raw Layer-2 Ethernet frames (DHCP discovers, ARP queries, IPv6 RS, mDNS) and records all source MAC addresses without requiring a DHCP server on the cable.
- **VM Support**: Tracks multiple MAC addresses simultaneously to wake hosts with bridged virtual machines. Up to 16 learned MACs are kept; when full, the least recently seen one is evicted.
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
  - `GET /log`: This run's log plus the end of the previous run's log (see [Debugging](#debugging-serial-console--logs)).
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
| **Console UART TX** | `GPIO43` | Row A, Pin 2 (silk `TX`) | `D6` (Pin 7) | — | UART0 log output, 115200 8N1 (to adapter RX) |
| **Console UART RX** | `GPIO44` | Row A, Pin 3 (silk `RX`) | `D7` (Pin 8) | — | UART0 console input (to adapter TX) |
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

## Debugging: Serial Console & Logs

The log goes to **both** UART0 (TX `GPIO43`, RX `GPIO44`, 115200 8N1; on a DevKitC-1 it's also on
the onboard USB-UART bridge) and the native USB CDC serial port, and both accept commands
(`help`, `status`, `log`, `prev`, `boot`, `reboot`, `bootloader`).

- All output is buffered in a 16 KB log ring that is never blocked by a slow or absent terminal. The
  USB serial port gets the backlog when a terminal opens it.
- The ring is kept in RAM that survives software, panic and watchdog resets, so after a crash or a
  hang that the watchdog recovered from, the boot log shows the reset reason and the end of the
  previous run's log is available with the `prev` command or `GET /log`. Unplugging the board loses
  it (see the checklist below).
- A `Health:` line (uptime, Wi-Fi state/IP/RSSI, Ethernet link, free heap) is logged every 10
  minutes, so the log shows the state leading up to a problem.
- A panic backtrace is printed by the ROM directly to UART0 only; capture UART0 to see it.

```bash
./02-monitor.sh               # USB serial console
./02-monitor.sh --uart /dev/ttyUSB0   # external USB-UART adapter on GPIO43/44
curl http://wol.local/log     # same log over Wi-Fi
```

### If it stops responding

1. **Try the IP address too**, not just `wol.local` (find it in the router's DHCP client list). If
   the IP works but the name doesn't, the problem is mDNS, not the board.
2. **Don't unplug it first**: a power cycle wipes the log. Connect to the USB serial port
   (`./02-monitor.sh`); the backlog shows up when the port opens, or type `log` to print it again.
   `status` shows the current Wi-Fi state.
3. **If the console doesn't respond either**, type `reboot` or press the RST button, then type
   `prev` (or open `GET /log`) to read the end of the hung run's log. Whether the log survives the
   RST button hasn't been verified; `reboot` and watchdog/panic resets keep it.
4. **For a crash backtrace**, capture UART0 with a USB-UART adapter; the panic handler doesn't
   write to the log ring.

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
  # Or request JSON response. If no packet could be sent, it returns HTTP 503 with
  # "status" set to "link_down" or "no_targets":
  curl -f -H "Accept: application/json" http://wol.local/wake
  ```

- **Query status and recorded MACs**:
  ```bash
  curl http://wol.local/status
  ```

- **Purge learned MACs**:
  ```bash
  curl -X POST http://wol.local/purge
  ```

---

## Future Tasks

- **Save crash core dumps to flash.** A panic backtrace currently only goes to UART0 at the moment
  of the crash, so it's lost unless a USB-UART adapter happens to be capturing it. Enabling
  ESP-IDF's core dump to flash (`CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH`, plus a `coredump` partition
  of ~64 KB in `partitions.csv`; the 8 MB flash has room after `storage`) would keep the backtrace
  across the reboot, to be read later over USB with `idf.py coredump-info`. Note that changing the
  partition table requires a full re-flash.
