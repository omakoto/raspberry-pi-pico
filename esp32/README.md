# ESP32 Firmware Development

This directory contains native C++ firmware projects, drivers, and utilities for **ESP32** and **ESP32-S3** microcontrollers built on the official **ESP-IDF** framework (`v5.3+` / `v5.5+` LTS).

---

## 1. Supported Hardware Targets

Unless specified otherwise in a project's README, projects in this directory target the **ESP32-S3** and are designed for:
- **Seeed Studio XIAO ESP32-S3**: Ultra-compact form factor (Reference: [`ref/ss-esp32-s3.md`](file:///home/omakoto/cbin/src/raspberry-pi-pico/ref/ss-esp32-s3.md)).
- **ESP32-S3-DevKitC-1** / YD-ESP32-S3 / NodeMCU-S3: Standard 44-pin dual USB-C development board (Reference: [`ref/esp32-s3-devkitc-1.md`](file:///home/omakoto/cbin/src/raspberry-pi-pico/ref/esp32-s3-devkitc-1.md)).
- **Seeed Studio XIAO ESP32-C6**: Compact RISC-V Wi-Fi 6 / Thread / Zigbee board (Reference: [`ref/ss-esp32-c6.md`](file:///home/omakoto/cbin/src/raspberry-pi-pico/ref/ss-esp32-c6.md)).

---

## 2. Standard Workflow Scripts

Every project in this directory follows a standardized script convention:

| Script | Purpose | Description |
| :--- | :--- | :--- |
| [`00-build.sh`](file:///home/omakoto/cbin/src/raspberry-pi-pico/esp32/2026-09-06-w5500-lan-test/00-build.sh) | **Build Firmware** | Automatically sources `${IDF_PATH}/export.sh` (or `~/esp-idf/export.sh`), unsets stale virtualenv variables, builds the application binary, and compiles FATFS filesystem images. |
| [`01-install.sh`](file:///home/omakoto/cbin/src/raspberry-pi-pico/esp32/2026-09-06-w5500-lan-test/01-install.sh) | **Flash Board** | Ensures binaries exist (triggering `00-build.sh` if needed), auto-detects active serial ports (`/dev/ttyACM*` or `/dev/ttyUSB*`), and flashes bootloader, partition table, app binary, and FATFS storage partition to the chip. Accepts optional port argument: `./01-install.sh /dev/ttyUSB0`. |
| [`02-monitor.sh`](file:///home/omakoto/cbin/src/raspberry-pi-pico/esp32/2026-09-06-w5500-lan-test/02-monitor.sh) | **Monitor Serial** | Connects to the console output over USB-UART bridge or native USB Serial/JTAG using `idf.py monitor`. Accepts optional port argument: `./02-monitor.sh /dev/ttyUSB0`. Exit with `Ctrl + ]`. |

---

## 3. Configuration Architecture (`config.toml` & FATFS)

Projects in this directory use a unified Wear Levelling FATFS architecture for storing runtime configuration without requiring hardcoded source edits or risky recompilation:

```text
+-------------------------------------------------------------------------+
| PC Build Environment                                                    |
|                                                                         |
|  fatfs_data/config.toml --------+                                        |
|  (Tracked base config)          |                                        |
|                                 v                                        |
|  $ESP32_CONFIG_TOML --------> CMakeLists.txt                             |
|  (External override)     (configure_file)                                |
|                                 |                                        |
|                                 v                                        |
|                     fatfs_data/config-override.toml                      |
|                                 |                                        |
|                                 v                                        |
|                  fatfs_create_spiflash_image()                           |
|                                 |                                        |
+---------------------------------|---------------------------------------+
                                  | (idf.py flash / 01-install.sh)
                                  v
+-------------------------------------------------------------------------+
| ESP32-S3 Flash Partition: "storage" (FATFS)                             |
|                                                                         |
|   /spiflash/config.toml          (Base defaults)                        |
|   /spiflash/config-override.toml (High-priority overrides)              |
|                                                                         |
|                      +------------------------+                         |
|                      |     ConfigManager      |                         |
|                      +------------------------+                         |
|                                  |                                      |
|                                  v                                      |
|                      Runtime Settings in C++                            |
+-------------------------------------------------------------------------+
```

### 1. Partition Table (`partitions.csv`)
Projects define a custom partition table in `partitions.csv` allocating space for a `storage` partition of type `data` and subtype `fat`:
```csv
# Name,   Type, SubType, Offset,   Size, Flags
nvs,      data, nvs,     0x9000,   0x6000,
phy_init, data, phy,     0xf000,   0x1000,
factory,  app,  factory, 0x10000,  0x200000,
storage,  data, fat,     0x210000, 0x200000,
```

### 2. Base Configuration (`fatfs_data/config.toml`)
Tracked in git under each project's `fatfs_data/` directory. Contains sensible defaults and commented documentation for pin mappings, ports, and hostnames.

### 3. External Override Mechanism (`$ESP32_CONFIG_TOML`)
To provide private credentials (Wi-Fi passwords, private keys, static IP configurations) without editing git-tracked files:
1. Export the environment variable pointing to your external TOML file:
   ```bash
   export ESP32_CONFIG_TOML="$HOME/.config/my-esp32-secrets.toml"
   ```
2. Run `./00-build.sh`.
3. CMake automatically copies the file to `fatfs_data/config-override.toml`, bundles it into `storage.bin`, and flashes it with `01-install.sh`.
4. Git ignores `config-override.toml` (enforced by `.gitignore`), preventing accidental secret leakage.

### 4. Runtime Layering (`ConfigManager`)
- During `app_main`, the application initializes `ConfigManager` which mounts the partition at `/spiflash` via `esp_vfs_fat_spiflash_mount_rw_wl`.
- `ConfigManager::load()` parses `/spiflash/config.toml` first, then loads `/spiflash/config-override.toml` on top, overriding any matching keys.
- Type-safe accessors (`get_string`, `get_int`, `get_bool`) read configuration values without relying on C++ exceptions (`-fno-exceptions` friendly).
- On projects featuring USB Mass Storage (such as `nsbackend-esp32s3` and `2026-09-06-w5500-lan-test`), `/spiflash` is also exposed directly to your PC via TinyUSB MSC as a USB flash drive for drag-and-drop configuration editing.

---

## 4. Hardware & Pinout Conventions

### Universal Pinout Compatibility
To maximize portability between the compact Seeed Studio XIAO ESP32-S3 and the 44-pin ESP32-S3-DevKitC-1, projects use universal GPIO assignments where possible:

| Function | ESP32-S3 GPIO | DevKitC-1 Physical Pin | Seeed Studio XIAO Pin | Details |
| :--- | :---: | :---: | :---: | :--- |
| **SPI SCLK** | `GPIO7` | Row B, Pin 7 (silk `7`) | `D8` (Pin 9) | Serial Clock |
| **SPI MISO** | `GPIO8` | Row B, Pin 12 (silk `8`) | `D9` (Pin 10) | Master In Slave Out |
| **SPI MOSI** | `GPIO9` | Row B, Pin 15 (silk `9`) | `D10` (Pin 11) | Master Out Slave In |
| **SPI CS** | `GPIO4` | Row B, Pin 4 (silk `4`) | `D3` (Pin 4) | Active-low chip select |
| **Hardware Reset** | `GPIO3` | Row B, Pin 13 (silk `3`) | `D2` (Pin 3) | Active-low peripheral reset |
| **Hardware Interrupt** | `GPIO2` | Row A, Pin 5 (silk `2`) | `D1` (Pin 2) | Active-low peripheral interrupt |
| **I2C SDA** | `GPIO5` | Row B, Pin 5 (silk `5`) | `D4` (Pin 5) | Hardware I2C data line |
| **I2C SCL** | `GPIO6` | Row B, Pin 6 (silk `6`) | `D5` (Pin 6) | Hardware I2C clock line |
| **Status LED** | `GPIO21` | Row A, Pin 18 (silk `21`) | Onboard Yellow LED | Active-low user indicator |
| **3.3V Power** | `3V3` | Row B, Pin 1 or 2 (silk `3V3`) | `3V3` (Pin 12) | 3.3V DC power rail |
| **Ground** | `GND` | Row A, Pin 1/21/22 or Row B, Pin 22 (silk `G`) | `GND` (Pin 13) | Common digital ground |

### Dual USB Port & Console Handling
- **`UART` Port (USB-UART Bridge)**: Located on the **Row A** side (near `RESET` button). Uses CP2102N or CH343 chip connected to `GPIO43` (TX) and `GPIO44` (RX). Enumerates as `/dev/ttyUSB*` (or `/dev/ttyACM*`).
- **`USB` Port (Native USB OTG)**: Located on the **Row B** side (near `BOOT` button). Connects directly to internal ESP32-S3 USB PHY on `GPIO19` (D-) and `GPIO20` (D+). In projects utilizing TinyUSB (`nsbackend-esp32s3`, `2026-09-06-w5500-lan-test`), this port exposes a composite USB device featuring:
  - **USB Mass Storage Class (MSC)**: The internal Wear Levelling `/spiflash` FATFS partition mounts on your PC as a removable drive, enabling direct editing of `config.toml`.
  - **USB CDC ACM Serial**: Enumerates as `/dev/ttyACM*` for serial monitoring and console interaction.
- **Dual Console Logging**: Firmware uses `dual_logger` to duplicate `ESP_LOG*` console output across both UART0 (`USB-C:UART`) and TinyUSB CDC ACM (`USB-C:USB`), ensuring `./02-monitor.sh` works seamlessly on whichever port you plug into.

---

## 5. Projects Directory Overview

| Project | Description |
| :--- | :--- |
| [`nsbackend-esp32s3`](file:///home/omakoto/cbin/src/raspberry-pi-pico/esp32/nsbackend-esp32s3) | High-performance Nintendo Switch USB HID Gamepad controller backend (`VID: 0x0f0d`, `PID: 0x0092`), composite TinyUSB stack (Gamepad + CDC Serial + MSC Flash Storage), multi-AP Wi-Fi manager, mDNS (`nscon.local`), streaming TCP command server on port `10100`, and physical debounced GPIO buttons. |
| [`2026-09-06-w5500-lan-test`](file:///home/omakoto/cbin/src/raspberry-pi-pico/esp32/2026-09-06-w5500-lan-test) | Hardwired TCP echo server over Ethernet using the USR-ES1 (WIZnet W5500) SPI module, composite TinyUSB stack (CDC ACM Serial + MSC Flash Storage), 160 ms hardware reset sequencing, automatic DHCP client, mDNS responder (`w5500-test.local` on port `10110`), periodic switch ARP priming, status LED patterns, and FATFS configuration. |
