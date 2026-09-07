# SHT31 Temperature & Humidity Sensor Test

A CircuitPython script to read ambient temperature (°C and °F) and relative humidity (%) from an **Sensirion SHT31-D** sensor over I2C on Raspberry Pi Pico, ESP32 / ESP32-S3, and compatible boards.

---

## Hardware Wiring

### Raspberry Pi Pico (Pico / Pico W / Pico 2 / Pico 2 W)

| Function | Pin Name | Default GPIO | Physical Pin # | Details |
| :--- | :--- | :--- | :--- | :--- |
| **I2C SCL** (sensor `SCL`) | GP11 | `GPIO11` | Pin 15 | I2C1 clock line |
| **I2C SDA** (sensor `SDA`) | GP10 | `GPIO10` | Pin 14 | I2C1 data line |
| **3.3V Power** (sensor `VIN` / `VCC`) | 3V3(OUT) | `3V3` | Pin 36 | 3.3V DC power supply |
| **Ground** (sensor `GND`) | GND | `GND` | Pin 3, 8, 13, 18, 23, 28, 38 | Common ground reference |

### ESP32-S3

| Function | ESP32-S3 GPIO | DevKitC-1 Physical Pin | Seeed Studio XIAO Pin | Details |
| :--- | :---: | :---: | :---: | :--- |
| **I2C SCL** (sensor `SCL`) | `GPIO6` | Row B, Pin 6 (silk `6`) | `D5` (Pin 6) | Hardware I2C clock line |
| **I2C SDA** (sensor `SDA`) | `GPIO5` | Row B, Pin 5 (silk `5`) | `D4` (Pin 5) | Hardware I2C data line |
| **3.3V Power** (sensor `VIN` / `VCC`) | `3V3` | Row B, Pin 1 or 2 (silk `3V3`) | `3V3` (Pin 12) | 3.3V DC power supply |
| **Ground** (sensor `GND`) | `GND` | Row A, Pin 1/21/22 or Row B, Pin 22 (silk `G`) | `GND` (Pin 13) | Common ground reference |

> *Note: SHT31 breakout boards typically include built-in 10kΩ pull-up resistors on SDA and SCL.*

---

## How It Works

1. **Auto-Detecting I2C Bus**:
   - Uses [`libs/common.py`](file:///home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/libs/common.py)'s `get_i2c()` helper to automatically detect the board's default hardware I2C peripheral (`board.I2C()` / `board.SCL` & `board.SDA`), with automatic fallback pairs for boards without predefined pins (e.g. Pico `GP11`/`GP10`).
2. **Measurement & CRC-8 Validation**:
   - Sends command `0x2400` (high repeatability measurement, no clock stretching) to address `0x44`.
   - Reads 6-byte response packet (2-byte temperature + 1-byte CRC, 2-byte humidity + 1-byte CRC).
   - Validates both checksums using CRC-8 polynomial $x^8 + x^5 + x^4 + 1$ (`0x31`) before converting raw data.

---

## How to Run

1. Run the script directly using `circuit-run`:
   ```bash
   /home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/2026-08-15-sht31-test/sht31-test.py
   ```
   Or copy to your `CIRCUITPY` drive:
   ```bash
   cp /home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/2026-08-15-sht31-test/sht31-test.py /run/media/$USER/CIRCUITPY/code.py
   ```

2. Open the serial console (`picocom -b 115200 /dev/ttyACM0`) to monitor sensor output:
   ```text
   SHT31 Temperature & Humidity Sensor Test Initialized.
   I2C bus successfully initialized.
   SHT31 sensor detected at I2C address 0x44.
   Temperature: 22.45 °C (72.41 °F), Humidity: 48.30 %
   Temperature: 22.46 °C (72.43 °F), Humidity: 48.28 %
   ```
