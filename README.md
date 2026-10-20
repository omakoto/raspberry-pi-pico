# Random Raspberry Pi Pico Projects

A collection of embedded firmware projects, drivers, and utilities for **Raspberry Pi Pico** (RP2040 / RP2350) and **ESP32** (ESP32-S3 / ESP32-C6) microcontroller families, spanning **C++ / FreeRTOS**, **ESP-IDF**, and **CircuitPython**.

---

## Hardware Pinout Documentation Rules

To ensure wiring diagrams and hardware connections are unambiguous and easy to follow when working on a breadboard or wiring custom enclosures, all pinout tables in project documentation throughout this repository must adhere to the following rules:

1. **Mandatory Pin Information**:
   - Every pinout table **must** include both the **logical pin name / GPIO number** (e.g., `GP0`, `GPIO0`, `SDA`, `TX`) and the **physical pin number** (`1`-indexed) on the board header.

2. **Raspberry Pi Pico Family Projects (`Pico`, `Pico W`, `Pico 2`, `Pico 2 W`)**:
   - Use standard **40-pin DIP** physical pin numbers (`Pin 1` through `Pin 40`) as defined in [`ref/pico.md`](ref/pico.md).
   - For internal onboard signals (such as the CYW43 wireless status LED), indicate `Onboard`.

3. **ESP32-S3 Projects**:
   - Because projects frequently target either standard dual-row development kits or ultra-compact micro-modules, pinout tables for ESP32-S3 projects **must** specify both:
     - **ESP32-S3-DevKitC-1 Pinout**: Standard 44-pin dual-row development kit pin numbering and header row (Row A/B, Pins 1–22) as documented in [`ref/esp32-s3-devkitc-1.md`](ref/esp32-s3-devkitc-1.md).
     - **Seeed Studio XIAO ESP32-S3 Pinout**: Compact 14-pin form factor specifying silkscreen label (e.g., `D0`–`D10`) and physical pin number (`Pin 1`–`Pin 14`) as documented in [`ref/ss-esp32-s3.md`](ref/ss-esp32-s3.md).

### Reference Table Templates

#### Raspberry Pi Pico Pinout Table Template
| Function | Pin Name | Default GPIO | Physical Pin # | Details |
| :--- | :--- | :--- | :--- | :--- |
| **Button A** | GP0 | `GPIO0` | Pin 1 | Active-low, internal pull-up |
| **I2C SDA** | GP20 | `GPIO20` | Pin 26 | I2C data line |
| **3.3V Power** | 3V3(OUT) | `3V3` | Pin 36 | 3.3V DC power rail |
| **Ground** | GND | `GND` | Pin 3, 8, 13, 18, 23, 28, 38 | Common digital ground |

#### ESP32-S3 Pinout Table Template
| Function | ESP32-S3 GPIO | DevKitC-1 Physical Pin | Seeed Studio XIAO Pin | Details |
| :--- | :---: | :---: | :---: | :--- |
| **SPI MOSI** | `GPIO9` | Row B, Pin 15 (silk `9`) | `D10` (Pin 11) | Master Out Slave In |
| **SPI SCLK** | `GPIO7` | Row B, Pin 7 (silk `7`) | `D8` (Pin 9) | Serial Clock |
| **Status LED** | `GPIO21` | Row A, Pin 18 (silk `21`) | Onboard Yellow LED | Active-low user indicator |

---

## Scripting Convention

All projects in this repository adhere to a standardized numbered workflow script convention:

| Script | Purpose | Description |
| :--- | :--- | :--- |
| `00-build.sh` | **Build Firmware** | Builds the application binary and any filesystem/storage images (e.g., CMake + Ninja for Pico SDK, `idf.py build` for ESP-IDF). Automatically sets up necessary SDK environments or paths. |
| `01-install.sh` | **Flash Board** | Flashes the compiled binaries and configuration images to the target board. Ensures build artifacts exist (invoking `00-build.sh` if needed) and auto-detects connected devices/ports or guides the user into bootloader mode. |
| `02-monitor.sh` | **Monitor Serial** | Connects to the serial console output (`/dev/ttyACM*` or `/dev/ttyUSB*`). **The monitor script must be a symlink to [`tools/monitor.sh`](tools/monitor.sh)** (e.g., `ln -s ../../tools/monitor.sh 02-monitor.sh`), which auto-detects active serial ports and connects via available terminal tools (`picocom`, `tio`, `minicom`, Python miniterm, or `screen`). |

### CircuitPython Projects

For CircuitPython projects where scripts are executed directly without an explicit compile step:
- Scripts are run via [`circuitpython/bin/circuit-run`](circuitpython/bin/circuit-run) (or via shebang `#!/usr/bin/env circuit-run`), which copies the script and any tagged `#file:` dependencies to the board volume as `code.py`.
- `circuit-run` automatically invokes [`tools/monitor.sh`](tools/monitor.sh) upon completion to stream serial output from the board.

---

## Configuration Files (`config.toml` and `config-override.toml`)

Projects of every kind here (ESP-IDF firmware, native Pico C++ firmware and CircuitPython scripts, not just ESP32) keep
their settings (Wi-Fi, pins, hostname, ...) in the same plain `key = value` TOML files, and one private override
file is shared by all of them. On a board the files sit on a small drive that the board shows over USB (a FAT
"config drive" for the C++ projects, the `CIRCUITPY` drive for CircuitPython), so a setting can be changed by
editing a file on a PC. The reference implementation for the ESP-IDF projects is
[`esp32/button-dispatcher`](esp32/button-dispatcher/); `wol-bridge` has the older copy without the secret hiding
below.

- **Two files.** `fatfs_data/config.toml` is the committed template (placeholders only). `fatfs_data/config-override.toml`
  is git-ignored and holds the real values. The firmware reads `config.toml` first and then the override; a key in
  the override replaces the same key in the base file, and keys in neither keep their compiled-in defaults.
- **One shared override for all projects.** Set `ESP32_CONFIG_TOML` to a TOML file outside the repository. ESP-IDF
  and Pico C++ builds copy it whole into their config drive image (for ESP-IDF, `configure_file` to
  `fatfs_data/config-override.toml` in the project's `CMakeLists.txt`).
  CircuitPython projects get it from [`circuit-run`](circuitpython/bin/circuit-run), which copies it to the board as
  `config-override.toml` (it also accepts `PICO_CONFIG_TOML`). The file therefore contains keys a given project never
  reads, such as another project's `pushover_token`, and every project must ignore keys it doesn't know.
- **Secrets are hidden at boot.** Anyone can read the drive by plugging the board in, so before the USB drive is
  first shown the firmware moves every secret into NVS and comments it out in the file as
  `# wifi_password = "***"`. A secret is any key ending in `_password` (optionally followed by digits, such as
  `wifi_password1`), `_key` or `_token`, whether or not the project uses it. Later boots read a commented-out
  secret from NVS, so the code just calls `config.get_string("wifi_password")`. To change a secret, write a new
  `key = "value"` line in either file; the next boot stores and hides it again. The file is rewritten only after
  the NVS write succeeded.
- **Only the ESP-IDF projects that use `config_manager` + `config_secrets` hide secrets.** On a CircuitPython board
  the whole override file, secrets included, is readable on the `CIRCUITPY` drive, and the other C++ projects (including
  the Pico ones) copy it as it is. Keep that in mind when a board is left somewhere others can plug it in.
- **Limit.** This keeps secrets off the drive, not off the flash chip: someone with `esptool` can still dump the
  flash unless flash encryption is enabled.

To use this in another project, copy `main/config_manager.{hpp,cpp}` and
`components/dispatcher_core/{include/dispatcher,src}/config_secrets.*` (plain C++, with host tests in
`test/test_config_secrets.cpp`), and use `configure_file` for the override as button-dispatcher does. Details:
[button-dispatcher README, Configuration](esp32/button-dispatcher/README.md#configuration-configtoml-config-overridetoml).

---

## Directory Overview

- **[`pico/`](pico/)**: Native C++ firmware projects for Raspberry Pi Pico / Pico 2 / Pico W / Pico 2 W built with Pico SDK, some on FreeRTOS SMP (e.g., [`nsbackend-pico`](pico/nsbackend-pico), [`otg-lan-test`](pico/otg-lan-test)).
- **[`esp32/`](esp32/)**: Native C++ firmware projects for ESP32-S3 and ESP32-C6 built with ESP-IDF (e.g., [`nsbackend-esp32s3`](esp32/nsbackend-esp32s3), [`2026-09-06-w5500-lan-test`](esp32/2026-09-06-w5500-lan-test)).
- **[`circuitpython/`](circuitpython/)**: CircuitPython scripts, peripheral drivers, and hardware test utilities.
- **[`ref/`](ref/)**: Hardware reference guides and pinout diagrams for Pico, DevKitC-1, and Seeed Studio XIAO boards.
