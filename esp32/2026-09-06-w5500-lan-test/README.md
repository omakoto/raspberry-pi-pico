# USR-ES1 (W5500) Ethernet TCP Server for ESP32-S3

A modern C++ implementation of the wired Ethernet TCP echo server for **ESP32-S3** development boards (specifically the **Seeed Studio XIAO ESP32-S3** and **ESP32-S3-DevKitC-1** / YD-ESP32-S3 / NodeMCU-S3) with the **USR-ES1 (WIZnet W5500)** SPI Ethernet module, built on the **ESP-IDF** framework (`v5.3+` / `v5.5+`).

This project is a high-performance C++ re-implementation of `circuitpython/2026-09-04-w5500-lan-test/`, utilizing the same FATFS configuration architecture as `esp32/nsbackend-esp32s3/`.

---

## 1. Overview & Features

- **Hardware W5500 SPI Ethernet**: Uses ESP-IDF's high-speed SPI Ethernet MAC driver (`esp_eth_mac_w5500`) with full LwIP TCP/IP stack integration.
- **Interrupt-driven Receive**: Frames are collected when the W5500 asserts `INTn` on `GPIO2`, with SPI polling available as a fallback when the interrupt line is not wired.
- **Hardware Reset Sequencing**: Executes an active-low hardware reset with a 160 ms stabilization window to allow the W5500 internal PLL clock to lock.
- **Automatic DHCP Client**: Requests and binds an IPv4 address upon Ethernet cable connection.
- **mDNS Service Responder**: Advertises the configured hostname as `<hostname>.local` (default: `w5500-test.local`) and registers service `_echo._tcp` on port `10110`.
- **Switch ARP Primer**: Automatically broadcasts a UDP announcement packet (`255.255.255.255:9`) upon boot and every 3 seconds while waiting for clients to update local switch and Wi-Fi bridge forwarding tables.
- **TCP Echo Server**: Listens on TCP port `10110` (configurable), logs incoming strings (UTF-8) or raw bytes to the console, and echoes data back to the client.
- **FATFS Configuration Storage**: Stores configuration in `config.toml` (and optional `config-override.toml`) on an internal Wear Levelling FATFS partition matching the `nsbackend-esp32s3` architecture.
- **Multi-Board Support**: Pre-configured for Seeed Studio XIAO ESP32-S3 and ESP32-S3-DevKitC-1 using universal pin mapping.
- **Status LED**: Indicates network lifecycle states (link down, DHCP acquisition, listening, client streaming).

---

## 2. Hardware Wiring & Connection Guides

### 2.1 Seeed Studio XIAO ESP32-S3 Wiring

#### Pin Connection Table

| USR-ES1 Pin | USR-ES1 Silk | XIAO ESP32-S3 Silk | ESP32-S3 GPIO | Signal Type | Description |
| :---: | :---: | :---: | :---: | :---: | :--- |
| **J2-2 / J2-3** | **3.3V / VIN** | **3V3** (Pin 12) | — | Power Input | **+3.3V Power** (requires $\ge 200\text{ mA}$) |
| **J1-1 / J2-1** | **GND** | **GND** (Pin 13) | — | Power Ground | **Common System Ground** |
| **J1-4** | **SCLK** | **D8** (Pin 9) | `GPIO7` | SPI Clock | SPI Clock driven by ESP32-S3 (20 MHz) |
| **J2-6** | **MISO** | **D9** (Pin 10) | `GPIO8` | SPI Data Out | SPI Master In / Slave Out |
| **J1-3** | **MOSI** | **D10** (Pin 11) | `GPIO9` | SPI Data In | SPI Master Out / Slave In |
| **J1-5** | **SCSn** | **D3** (Pin 4) | `GPIO4` | SPI Chip Select| Active-LOW SPI Chip Select (CS) |
| **J2-5** | **RSTn** | **D2** (Pin 3) | `GPIO3` | Control Input | Active-LOW Hardware Reset |
| **J1-6** | **INTn** | **D1** (Pin 2) | `GPIO2` | Interrupt Out | Active-LOW Hardware Interrupt (or Polling) |
| **J2-4** | **NC / PWDN**| — | — | Control Input | Leave floating or connect to GND |
| — | — | **LED** (Onboard) | `GPIO21` | Status Output | Active-LOW Yellow User LED |

> [!CAUTION]
> The USR-ES1 module **does NOT have an onboard 5V regulator**. Connect its power pins (`J2-2` / `J2-3`) strictly to the **3.3V (3V3)** pin of the XIAO ESP32-S3. Never connect 5V or USB VBUS to the module.

#### ASCII Connection Diagram (XIAO ESP32-S3)

```text
    Seeed Studio XIAO ESP32-S3                       USR-ES1 (W5500 Module)
     +------------------------+                    +------------------------+
     |       [ USB-C ]        |                    |   [ RJ45 ETHERNET ]    |
     |                        |                    +---+----------------+---+
     | D0                 5V  |                        |                |
     | D1 (GPIO2) --------+----------------------------| J1-6 (INTn)    |
     | D2 (GPIO3) --------+----------------------------| J2-5 (RSTn)    |
     | D3 (GPIO4) --------+----------------------------| J1-5 (SCSn/CS) |
     | D4                 D10 | -- (GPIO9 / MOSI) -----| J1-3 (MOSI)    |
     | D5                 D9  | -- (GPIO8 / MISO) -----| J2-6 (MISO)    |
     | D6                 D8  | -- (GPIO7 / SCK)  -----| J1-4 (SCLK)    |
     |      [ LED:IO21 ]  3V3 | -----------------------| J2-2 (3.3V)    |
     |                    GND | -----------------------| J1-1 (GND)     |
     +------------------------+                    +------------------------+
```

---

### 2.2 ESP32-S3-DevKitC-1 (Dual USB-C) Wiring

The **ESP32-S3-DevKitC-1** (and compatible 44-pin boards such as **YD-ESP32-S3** and **NodeMCU-S3**) features two 22-pin headers:
- **Row A (Top Header)**: Near the CP2102N USB-UART bridge port and RESET button.
- **Row B (Bottom Header)**: Near the native USB OTG port and BOOT button.

Reference document: [`ref/esp32-s3-devkitc-1.md`](file:///home/omakoto/cbin/src/raspberry-pi-pico/ref/esp32-s3-devkitc-1.md).

#### Option A: Universal Drop-in Pinout (Recommended - Zero Config Changes)

This wiring uses GPIO pins that directly match the project's default configuration, so **no changes to `config.toml` are required**:

| USR-ES1 Pin | USR-ES1 Silk | DevKitC-1 Header | Header Pin # | Silk Label | ESP32-S3 GPIO | Description |
| :---: | :---: | :---: | :---: | :---: | :---: | :--- |
| **J2-2 / J2-3** | **3.3V** | **Row B** | Pin 1 or 2 | `3V3` | — | 3.3V Power Rail (from onboard LDO) |
| **J1-1 / J2-1** | **GND** | **Row A / B** | Row B Pin 22 (or Row A Pin 1/21/22) | `G` | — | Common System Ground |
| **J1-4** | **SCLK** | **Row B** | Pin 7 | `7` | `GPIO7` | SPI Clock (20 MHz) |
| **J2-6** | **MISO** | **Row B** | Pin 12 | `8` | `GPIO8` | SPI Master In / Slave Out |
| **J1-3** | **MOSI** | **Row B** | Pin 15 | `9` | `GPIO9` | SPI Master Out / Slave In |
| **J1-5** | **SCSn** | **Row B** | Pin 4 | `4` | `GPIO4` | Active-LOW SPI Chip Select (CS) |
| **J2-5** | **RSTn** | **Row B** | Pin 13 | `3` | `GPIO3` | Active-LOW Hardware Reset |
| **J1-6** | **INTn** | **Row A** | Pin 5 | `2` | `GPIO2` | Active-LOW Hardware Interrupt |
| **J2-4** | **PWDN** | — | — | — | — | Leave floating or tied to GND |

#### Option B: Dedicated Hardware FSPI Block Pinout

If you prefer using the consecutive FSPI block on **Row B**, configure the pins in `fatfs_data/config.toml`:

| USR-ES1 Pin | Signal | DevKitC-1 Header | Header Pin # | Silk Label | ESP32-S3 GPIO | Notes in `config.toml` |
| :---: | :---: | :---: | :---: | :---: | :---: | :--- |
| **J1-4** | **SCLK** | **Row B** | Pin 18 | `12` | `GPIO12` | `spi_sck = 12` |
| **J2-6** | **MISO** | **Row B** | Pin 19 | `13` | `GPIO13` | `spi_miso = 13` |
| **J1-3** | **MOSI** | **Row B** | Pin 17 | `11` | `GPIO11` | `spi_mosi = 11` |
| **J1-5** | **SCSn** | **Row B** | Pin 16 | `10` | `GPIO10` | `spi_cs = 10` |
| **J2-5** | **RSTn** | **Row B** | Pin 15 | `9` | `GPIO9` | `spi_reset = 9` |
| **J1-6** | **INTn** | **Row B** | Pin 13 | `3` | `GPIO3` | `spi_int = 3` |
| **J2-2 / J2-3** | **3.3V** | **Row B** | Pin 1 or 2 | `3V3` | — | Direct 3.3V supply |
| **J1-1 / J2-1** | **GND** | **Row B** | Pin 22 | `G` | — | System ground |

---

##### ASCII Connection Diagram (ESP32-S3-DevKitC-1 - Universal Option A)

```text
                                     ESP32-S3-DevKitC-1
            +-------------------------------------------------------------------------+
[Row A: Top]| (G) (TX)(RX)(1) [2] (42)...                                    (G)  (G) |
            |                  | (Pin 5 / GPIO2)                                      |
            |                  +------------------------------------------------+     |
            |                                                                   |     |
            |  [PCB ANT]      +------------------+              [UART Bridge]   |     |
            |                 |  ESP32-S3-WROOM  |              [USB-C:UART ]   |     |
            |                 +------------------+                              |     |
            |                                                   [Native USB ]   |     |
            |                                                   [USB-C:USB  ]   |     |
            |  [3V3]     [4]        [7]            [8] [3]       [9]        (G) |     |
[Row B: Btm]|  (1/2)     (4)        (7)            (12)(13)      (15)       (22)|     |
            +----+--------+----------+--------------+---+---------+----------+--+-----+
                 |        |          |              |   |         |          |  |
                 |        |          |              |   |         |          |  +-- [INTn]  J1-6 (GPIO2)
                 |        |          |              |   |         |          +----- [GND]   J1-1 / J2-1
                 |        |          |              |   |         +---------------- [MOSI]  J1-3 (GPIO9)
                 |        |          |              |   +-------------------------- [RSTn]  J2-5 (GPIO3)
                 |        |          |              +------------------------------ [MISO]  J2-6 (GPIO8)
                 |        |          +--------------------------------------------- [SCLK]  J1-4 (GPIO7)
                 |        +-------------------------------------------------------- [SCSn]  J1-5 (GPIO4)
                 +----------------------------------------------------------------- [3.3V]  J2-2 / J2-3
                                                                              +--------------------+
                                                                              | USR-ES1 (W5500)    |
                                                                              | [ RJ45 ETHERNET ]  |
                                                                              +--------------------+
```

---

## 3. Status LED Patterns

On the **Seeed Studio XIAO ESP32-S3**, the onboard yellow user LED (`GPIO21`, active LOW) indicates network lifecycle states:

| LED Pattern | Lifecycle State | Description |
| :--- | :--- | :--- |
| **Solid ON** | **Initializing** | Boot, NVS mount, FATFS load, and W5500 hardware reset |
| **0.1s ON, 0.1s OFF** (Rapid strobe) | **Link DOWN** | Ethernet cable is disconnected / PHY link down |
| **0.1s ON, 0.9s OFF** (Slow pulse) | **DHCP Acquiring** | Cable connected (Link UP), awaiting DHCP lease |
| **0.5s ON, 0.5s OFF** (Medium blink) | **Waiting for Client** | IP acquired, mDNS active, TCP server listening |
| **0.9s ON, 0.1s OFF** (Heartbeat) | **Client Connected** | TCP client connected and actively streaming |

*(Note: On the ESP32-S3-DevKitC-1, `GPIO21` is exposed on Row A Pin 18 and can be connected to an external LED with a current-limiting resistor, or left disconnected).*

---

## 4. Configuration Architecture (`fatfs_data/`)

Following the architecture of `esp32/nsbackend-esp32s3/`, runtime configuration is loaded from a Wear Levelling FATFS partition (`storage`):

### 1. Base Configuration (`fatfs_data/config.toml`)

Edit [`fatfs_data/config.toml`](file:///home/omakoto/cbin/src/raspberry-pi-pico/esp32/2026-09-06-w5500-lan-test/fatfs_data/config.toml) to customize device settings:

```toml
# w5500-lan-test configuration
hostname = "w5500-test"
tcp_port = 10110
mac = "DE:AD:BE:EF:FE:ED"

# Hardware SPI Pin Configuration
# -------------------------------------------------------------
# Default Pinout (Universal / XIAO ESP32-S3 & DevKitC-1 compatible):
#   spi_sck   = 7    # XIAO: D8 (Pin 9)  | DevKitC-1: Row B, silk '7' (Pin 7)
#   spi_mosi  = 9    # XIAO: D10 (Pin 11)| DevKitC-1: Row B, silk '9' (Pin 15)
#   spi_miso  = 8    # XIAO: D9 (Pin 10) | DevKitC-1: Row B, silk '8' (Pin 12)
#   spi_cs    = 4    # XIAO: D3 (Pin 4)  | DevKitC-1: Row B, silk '4' (Pin 4)
#   spi_reset = -1   # -1 if RSTn is unconnected (or 3 to drive it)
#   spi_int   = 2    # XIAO: D1 (Pin 2)  | DevKitC-1: Row A, silk '2' (Pin 5)
#   poll_period_ms = 2 # Only used when spi_int = -1
#   int_diag = false # Log INTn assert durations for RX path diagnosis
```

### Interrupt vs. Polling Receive

`spi_int` selects how the driver learns that a frame has arrived:

| `spi_int` | Mode | Behaviour |
| :--- | :--- | :--- |
| `2` (default) | **Interrupt** | The W5500 asserts `INTn` on the configured GPIO and the MAC receive task is woken immediately. `poll_period_ms` is ignored. |
| `-1` | **Polling** | The driver reads the chip every `poll_period_ms` milliseconds. Use this when `INTn` is not wired. |

Interrupt mode requires the module's `J1-6` (`INTn`) pin to be physically connected to
`GPIO2`. If it is not wired, the pin floats and no receive events are delivered, so set
`spi_int = -1` instead.

If `gpio_install_isr_service()` cannot be started, the driver logs a warning and falls back
to polling rather than failing to bring up the network.

#### Verifying the interrupt path

Set `int_diag = true` to have the driver sample `INTn` and report, every 10 seconds, how
long the line was left asserted:

```text
I (20401) W5500Driver: INTn diag: 11 assertions, longest assert 7000 us
```

The W5500 holds `INTn` LOW until the driver clears its socket interrupt register, so this
measures how promptly the receive path is being serviced, independently of any network
measurement. A healthy link clears the line within a few milliseconds. ESP-IDF's MAC
receive task also wakes on a one-second timeout as a safety net, so `longest assert` values
approaching `1000000 us` indicate interrupts are being missed and the link is only limping
along on that fallback — check the `INTn` wiring and the `spi_int` GPIO number.

### 2. External Config Override (`$ESP32_CONFIG_TOML`)

You can provide an external override file without modifying tracked git files:

```bash
export ESP32_CONFIG_TOML="/path/to/my-custom-config.toml"
./00-build.sh
```

During build, CMake automatically copies this file into `fatfs_data/config-override.toml` and embeds it in `storage.bin`. Settings in `config-override.toml` take precedence over `config.toml`.

---

## 5. Building & Flashing

### Prerequisites

Ensure ESP-IDF (`v5.3+` or `v5.5+`) is installed at `~/esp-idf` (or `$IDF_PATH` is set).

### 1. Build Firmware & Filesystem Image

Run [`./00-build.sh`](file:///home/omakoto/cbin/src/raspberry-pi-pico/esp32/2026-09-06-w5500-lan-test/00-build.sh):

```bash
cd ~/cbin/src/raspberry-pi-pico/esp32/2026-09-06-w5500-lan-test
./00-build.sh
```

This compiles `w5500-lan-test.bin` and generates the FATFS image `storage.bin` from `fatfs_data/`.

### 2. Flash to ESP32-S3

Connect either USB port:
- **XIAO ESP32-S3**: Connect USB-C port. Put into Bootloader mode (Hold **B**, press & release **R**, release **B**).
- **ESP32-S3-DevKitC-1**: Connect to the `UART` USB-C port (or `USB` native port). Put into Bootloader mode (Hold **BOOT**, press & release **RESET**, release **BOOT**).

Run [`./01-install.sh`](file:///home/omakoto/cbin/src/raspberry-pi-pico/esp32/2026-09-06-w5500-lan-test/01-install.sh):

```bash
./01-install.sh
# or specify the port explicitly (e.g. /dev/ttyUSB0 or /dev/ttyACM0):
./01-install.sh /dev/ttyUSB0
```

### 3. Monitor Serial Console

Run [`./02-monitor.sh`](file:///home/omakoto/cbin/src/raspberry-pi-pico/esp32/2026-09-06-w5500-lan-test/02-monitor.sh):

```bash
./02-monitor.sh
# or specify the port explicitly:
./02-monitor.sh /dev/ttyUSB0
# Exit monitor with: Ctrl + ]
```

### 4. USB Mass Storage (MSC) & Dual Console Logging

Like [`esp32/nsbackend-esp32s3`](file:///home/omakoto/cbin/src/raspberry-pi-pico/esp32/nsbackend-esp32s3), this firmware integrates a TinyUSB composite device providing both **USB Mass Storage (MSC)** and **CDC ACM Serial**:

- **USB Flash Drive**: When plugged into the **Native USB port** (`USB-C:USB` on DevKitC-1, or the single USB-C port on XIAO), the wear-levelling FATFS `/spiflash` partition enumerates on your PC as a removable USB storage drive. You can directly inspect and modify `config.toml` from your computer; changes take effect upon the next reboot.
- **Dual Console Logging**: Serial logs (`ESP_LOG*`) are mirrored in real time to **both** the hardware UART bridge (`USB-C:UART`) and the TinyUSB CDC ACM serial port (`USB-C:USB`). You can run `./02-monitor.sh` on whichever port you plug in.

---

## 6. Testing & Verification

1. When the board boots, verify the serial output:

```text
I (310) Main: Starting W5500 LAN TCP Server on Seeed Studio XIAO ESP32-S3 / DevKitC-1...
I (320) ConfigManager: FATFS partition 'storage' mounted successfully at '/spiflash'
I (325) ConfigManager: Loaded base configuration from '/spiflash/config.toml'
I (330) Main: Configuration: hostname='w5500-test', tcp_port=10110, mac='DE:AD:BE:EF:FE:ED'
I (340) Main: SPI Pins: SCK=GPIO7, MOSI=GPIO9, MISO=GPIO8, CS=GPIO4, RST=GPIO3, INT=GPIO2
I (350) W5500Driver: Executing hardware reset pulse on W5500 (RST GPIO 3)...
I (520) W5500Driver: W5500 hardware reset completed and PLL stabilized.
I (530) W5500Driver: Initializing SPI bus: SCLK=GPIO7, MOSI=GPIO9, MISO=GPIO8, CS=GPIO4
I (540) W5500Driver: Configuring W5500 in Interrupt mode on GPIO 2
I (550) W5500Driver: Configured custom MAC address: DE:AD:BE:EF:FE:ED
I (560) W5500Driver: Checking physical Ethernet link state...
I (1820) W5500Driver: Ethernet cable connected (link is UP)! Requesting DHCP lease...
------------------------------------------------
Ethernet Connected!
  MAC Address:  DE:AD:BE:EF:FE:ED
  IP Address:   192.168.1.125
  Subnet Mask:  255.255.255.0
  Gateway:      192.168.1.1
  DNS Server:   192.168.1.1
  Hostname:     w5500-test
------------------------------------------------
I (2400) MdnsService: MDNS hostname set to: w5500-test.local
I (2410) MdnsService: MDNS service registered: _echo._tcp on port 10110
I (2420) TcpServer: TCP Echo Server listening on port 10110
I (2430) Main: Connect via IP:   nc 192.168.1.125 10110
I (2440) Main: Connect via mDNS: nc w5500-test.local 10110
```

2. Connect from your computer via netcat using either the DHCP IP or mDNS:

```bash
nc w5500-test.local 10110
# or:
nc 192.168.1.125 10110
```

3. Type any message and press Enter; the server will echo your message back:

```text
hello world
hello world
```
