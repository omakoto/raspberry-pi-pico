# SHT31 Temperature & Humidity Sensor Test

A CircuitPython script to read ambient temperature (°C and °F) and relative humidity (%) from an **Sensirion SHT31-D** sensor over I2C on Raspberry Pi Pico, ESP32 / ESP32-S3, and compatible boards.

---

## Hardware Wiring

| Sensor Pin | Function | Raspberry Pi Pico | Seeed Studio XIAO ESP32-S3 | Description |
| :--- | :--- | :--- | :--- | :--- |
| **VIN / VCC** | Power | **3V3 (Out)** (Pin 36) | **3V3** | 3.3V Power supply |
| **GND** | Ground | **GND** (Pin 38 / 23) | **GND** | Common ground reference |
| **SCL** | I2C Clock | **GP11** (Pin 15) | **D5 / IO6** (Pin 6) | I2C SCL line |
| **SDA** | I2C Data | **GP10** (Pin 14) | **D4 / IO5** (Pin 5) | I2C SDA line |

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
