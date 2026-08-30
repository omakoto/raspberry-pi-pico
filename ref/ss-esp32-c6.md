# Seeed Studio XIAO ESP32-C6 Reference & Pinout

A compact ESP32-C6 development board featuring a high-performance 32-bit RISC-V core (up to 160 MHz) alongside a low-power (LP) 32-bit RISC-V core (up to 20 MHz), 4MB Flash, 512KB SRAM, Wi-Fi 6 (802.11ax), Bluetooth 5.3 (BLE), Zigbee 3.0, Thread / IEEE 802.15.4 (Matter native), and USB-C.

---

## ASCII Pinout Diagram

```text
                           +------------------------+
                           |       [ USB-C ]        |
                           |                        |
        [RESET] ( )        |       +--------+       |        ( ) [BOOT / IO9]
        Button             |       | ESP32  |       |        Button
                           |       | -C6    |       |
                           |       +--------+       |
                           |                        |
(IO0  / A0 / LP_IO0)   D0 -| [1]                [14]|- 5V (VBUS / VIN)
(IO1  / A1 / LP_IO1)   D1 -| [2]    Seeed       [13]|- GND
(IO2  / A2 / LP_IO2)   D2 -| [3]    Studio      [12]|- 3V3 (Output)
(IO21 / A3 / LP_IO21)  D3 -| [4]    XIAO        [11]|- D10 (IO19 / MOSI)
(IO22 / A4 / SDA)      D4 -| [5]   ESP32-C6     [10]|- D9  (IO20 / MISO)
(IO23 / A5 / SCL)      D5 -| [6]                 [9]|- D8  (IO18 / SCK)
(IO16 / UART TX)       D6 -| [7]     [ LED ]     [8]|- D7  (IO17 / UART RX)
                           |         (IO15)         |
                           +------------------------+
                                     |  |
                                  [Antenna]
```

---

## Pin Mapping Table

| Pin # | Silk Label | ESP32-C6 GPIO | CircuitPython (`board.*`) | Analog / ADC | Default Function / Protocols |
| :---: | :---: | :---: | :---: | :---: | :--- |
| **1** | **D0** | `GPIO0` | `board.D0` / `board.A0` / `board.IO0` | `ADC1_CH0` | Low Power GPIO (`LP_GPIO0`) |
| **2** | **D1** | `GPIO1` | `board.D1` / `board.A1` / `board.IO1` | `ADC1_CH1` | Low Power GPIO (`LP_GPIO1`) |
| **3** | **D2** | `GPIO2` | `board.D2` / `board.A2` / `board.IO2` | `ADC1_CH2` | Low Power GPIO (`LP_GPIO2`) |
| **4** | **D3** | `GPIO21`| `board.D3` / `board.A3` / `board.IO21`| `ADC1_CH4` | Low Power GPIO (`LP_GPIO21`)|
| **5** | **D4** | `GPIO22`| `board.D4` / `board.A4` / `board.SDA` | — | **I2C SDA** (Hardware `I2C0`) / `LP_GPIO22` |
| **6** | **D5** | `GPIO23`| `board.D5` / `board.A5` / `board.SCL` | — | **I2C SCL** (Hardware `I2C0`) / `LP_GPIO23` |
| **7** | **D6** | `GPIO16`| `board.D6` / `board.TX` / `board.IO16`| — | **UART TX** (`UART0`) |
| **8** | **D7** | `GPIO17`| `board.D7` / `board.RX` / `board.IO17`| — | **UART RX** (`UART0`) |
| **9** | **D8** | `GPIO18`| `board.D8` / `board.SCK` / `board.IO18`| — | **SPI SCK** (`SPI2`) |
| **10** | **D9** | `GPIO20`| `board.D9` / `board.MISO` / `board.IO20`| — | **SPI MISO** (`SPI2`) |
| **11** | **D10**| `GPIO19`| `board.D10` / `board.MOSI` / `board.IO19`| — | **SPI MOSI** (`SPI2`) |
| **12** | **3V3**| — | — | — | 3.3V Regulated Power Output |
| **13** | **GND**| — | — | — | Common Ground |
| **14** | **5V** | — | — | — | 5V Power Supply Input / VBUS Output |

---

## On-Board Components & Special Pins

| Component | Pin / GPIO | Description |
| :--- | :--- | :--- |
| **User LED (Yellow)** | `GPIO15` (`board.LED`) | Active LOW (driven LOW to turn ON) |
| **Charge LED (Red)** | Hardware Controlled | Illuminates when charging battery via bottom pads |
| **BOOT Button** | `GPIO9` (`board.BUTTON`) | Pulled HIGH, connected to GND when pressed |
| **RESET Button** | Hardware `EN` / `CHIP_PU` | Hardware reset line |
| **Battery Pads (Bottom)** | `BAT+` / `BAT-` | Supports 3.7V Lithium battery input and charging |

---

## CircuitPython Peripherals Usage

### I2C Bus

The default hardware I2C bus is wired to `D4` (SDA / IO22) and `D5` (SCL / IO23):

```python
import board
import busio

# Default board I2C singleton:
i2c = board.I2C()  # Uses board.SCL (IO23/D5) and board.SDA (IO22/D4)

# Or explicitly via busio:
i2c = busio.I2C(scl=board.D5, sda=board.D4)
```

### SPI Bus

The default SPI bus is mapped to `D8` (SCK / IO18), `D9` (MISO / IO20), and `D10` (MOSI / IO19):

```python
import board
import busio

spi = board.SPI()  # Uses board.SCK (IO18), board.MOSI (IO19), board.MISO (IO20)
```

### UART Serial

```python
import board
import busio

uart = busio.UART(tx=board.TX, rx=board.RX, baudrate=115200)
```
