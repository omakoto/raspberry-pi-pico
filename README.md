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

## Directory Overview

- **[`pico/`](pico/)**: Native C++ firmware projects for Raspberry Pi Pico / Pico 2 / Pico W / Pico 2 W built with Pico SDK and FreeRTOS SMP (e.g., [`nsbackend-pico`](pico/nsbackend-pico)).
- **[`esp32/`](esp32/)**: Native C++ firmware projects for ESP32-S3 and ESP32-C6 built with ESP-IDF (e.g., [`nsbackend-esp32s3`](esp32/nsbackend-esp32s3), [`2026-09-06-w5500-lan-test`](esp32/2026-09-06-w5500-lan-test)).
- **[`circuitpython/`](circuitpython/)**: CircuitPython scripts, peripheral drivers, and hardware test utilities.
- **[`ref/`](ref/)**: Hardware reference guides and pinout diagrams for Pico, DevKitC-1, and Seeed Studio XIAO boards.
