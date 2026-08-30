# Seeed Studio XIAO ESP32-S3 Reference & Pinout

A compact ESP32-S3 development board featuring dual-core Xtensa 32-bit LX7 CPU (up to 240 MHz), 8MB PSRAM, 8MB Flash, Wi-Fi 4 (802.11 b/g/n), Bluetooth 5.0 (BLE), and native USB-C.

---

## ASCII Pinout Diagram

```text
                           +------------------------+
                           |       [ USB-C ]        |
                           |                        |
        [RESET] ( )        |       +--------+       |        ( ) [BOOT / IO0]
        Button             |       | ESP32  |       |        Button
                           |       | -S3    |       |
                           |       +--------+       |
                           |                        |
(IO1 / A0 / Touch 1)   D0 -| [1]                [14]|- 5V (VBUS / VIN)
(IO2 / A1 / Touch 2)   D1 -| [2]    Seeed       [13]|- GND
(IO3 / A2 / Touch 3)   D2 -| [3]    Studio      [12]|- 3V3 (Output)
(IO4 / A3 / Touch 4)   D3 -| [4]    XIAO        [11]|- D10 (IO9 / MOSI)
(IO5 / A4 / SDA)       D4 -| [5]   ESP32-S3     [10]|- D9  (IO8 / MISO)
(IO6 / A5 / SCL)       D5 -| [6]                 [9]|- D8  (IO7 / SCK)
(IO43 / UART TX)       D6 -| [7]     [ LED ]     [8]|- D7  (IO44 / UART RX)
                           |         (IO21)         |
                           +------------------------+
                                     |  |
                                  [Antenna]
```

---

## Pin Mapping Table

| Pin # | Silk Label | ESP32-S3 GPIO | CircuitPython (`board.*`) | Analog / ADC | Default Function / Protocols |
| :---: | :---: | :---: | :---: | :---: | :--- |
| **1** | **D0** | `GPIO1` | `board.D0` / `board.A0` / `board.IO1` | `ADC1_CH0` | Touch 1 / GPIO |
| **2** | **D1** | `GPIO2` | `board.D1` / `board.A1` / `board.IO2` | `ADC1_CH1` | Touch 2 / GPIO |
| **3** | **D2** | `GPIO3` | `board.D2` / `board.A2` / `board.IO3` | `ADC1_CH2` | Touch 3 / GPIO |
| **4** | **D3** | `GPIO4` | `board.D3` / `board.A3` / `board.IO4` | `ADC1_CH3` | Touch 4 / GPIO |
| **5** | **D4** | `GPIO5` | `board.D4` / `board.A4` / `board.SDA` | `ADC1_CH4` | **I2C SDA** (Hardware `I2C0` / `I2C1`) / Touch 5 |
| **6** | **D5** | `GPIO6` | `board.D5` / `board.A5` / `board.SCL` | `ADC1_CH5` | **I2C SCL** (Hardware `I2C0` / `I2C1`) / Touch 6 |
| **7** | **D6** | `GPIO43` | `board.D6` / `board.TX` / `board.IO43`| — | **UART TX** (`UART0`) |
| **8** | **D7** | `GPIO44` | `board.D7` / `board.RX` / `board.IO44`| — | **UART RX** (`UART0`) |
| **9** | **D8** | `GPIO7` | `board.D8` / `board.SCK` / `board.IO7` | `ADC1_CH6` | **SPI SCK** / Touch 7 |
| **10** | **D9** | `GPIO8` | `board.D9` / `board.MISO` / `board.IO8`| `ADC1_CH7` | **SPI MISO** / Touch 8 |
| **11** | **D10**| `GPIO9` | `board.D10` / `board.MOSI` / `board.IO9`| `ADC1_CH8` | **SPI MOSI** / Touch 9 |
| **12** | **3V3**| — | — | — | 3.3V Regulated Power Output |
| **13** | **GND**| — | — | — | Common Ground |
| **14** | **5V** | — | — | — | 5V Power Supply Input / VBUS Output |

---

## On-Board Components & Special Pins

| Component | Pin / GPIO | Description |
| :--- | :--- | :--- |
| **User LED (Yellow)** | `GPIO21` (`board.LED`) | Active LOW (driven LOW to turn ON) |
| **Charge LED (Red)** | Hardware Controlled | Illuminates when charging battery via bottom pads |
| **BOOT Button** | `GPIO0` (`board.BUTTON`) | Pulled HIGH, connected to GND when pressed |
| **RESET Button** | Hardware `EN` / `CHIP_PU` | Hardware reset line |
| **Battery Pads (Bottom)** | `BAT+` / `BAT-` | Supports 3.7V Lithium battery input and charging |

---

## CircuitPython Peripherals Usage

### I2C Bus

The default hardware I2C bus is wired to `D4` (SDA) and `D5` (SCL):

```python
import board
import busio

# Default board I2C singleton:
i2c = board.I2C()  # Uses board.SCL (IO6/D5) and board.SDA (IO5/D4)

# Or explicitly via busio:
i2c = busio.I2C(scl=board.D5, sda=board.D4)
```

### SPI Bus

The default SPI bus is mapped to `D8` (SCK), `D9` (MISO), and `D10` (MOSI):

```python
import board
import busio

spi = board.SPI()  # Uses board.SCK (IO7), board.MOSI (IO9), board.MISO (IO8)
```

### UART Serial

```python
import board
import busio

uart = busio.UART(tx=board.TX, rx=board.RX, baudrate=115200)
```
