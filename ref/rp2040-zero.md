# RP2040-Zero Reference & Pinout

A compact development board based on the Raspberry Pi **RP2040** microcontroller (Waveshare RP2040-Zero and compatible 23-pin U-layout variants, e.g. Amazon ASIN [B0DXL12W59](https://www.amazon.com/dp/B0DXL12W59)).

Features a dual-core ARM Cortex-M0+ (up to 133 MHz), 264KB SRAM, 2MB QSPI Flash, onboard WS2812 RGB LED, BOOT & RESET buttons, and a USB-C connector with castellated edge pads (9 pins left, 5 pins bottom, 9 pins right = 23 pins total).

---

## ASCII Pinout Diagram

```text
                           +------------------------+
                           |       [ USB-C ]        |
                           |                        |
        [BOOT] ( )         |      +----------+      |         ( ) [RESET]
        Button             |      |  RP2040  |      |         Button
                           |      | Dual M0+ |      |
                           |      +----------+      |
                           |                        |
                           |      [WS2812 RGB]      |
                           |         (GP16)         |
                           |                        |
(5V VBUS Input)        5V -| [L1]              [R1] |- 0  (UART0 TX / I2C0 SDA / SPI0 RX)
(Ground)              GND -| [L2]              [R2] |- 1  (UART0 RX / I2C0 SCL / SPI0 CS)
(3.3V Regulated Out)  3V3 -| [L3]     RP2040   [R3] |- 2  (UART1 TX / I2C1 SDA / SPI0 SCK)
(ADC3 / I2C0 SCL)    GP29 -| [L4]      ZERO    [R4] |- 3  (UART1 RX / I2C1 SCL / SPI0 TX)
(ADC2 / I2C0 SDA)    GP28 -| [L5]   (23-Pin    [R5] |- 4  (UART1 CTS / I2C0 SDA / SPI0 RX)
(ADC1 / I2C1 SCL)    GP27 -| [L6]   U-Layout)  [R6] |- 5  (UART1 RTS / I2C0 SCL / SPI0 CS)
(ADC0 / I2C1 SDA)    GP26 -| [L7]              [R7] |- 6  (UART1 TX / I2C1 SDA / SPI0 SCK)
(SPI1 TX / I2C1 SCL) GP15 -| [L8]              [R8] |- 7  (UART1 RX / I2C1 SCL / SPI0 TX)
(SPI1 SCK / I2C1 SDA)GP14 -| [L9]              [R9] |- 8  (UART1 CTS / I2C0 SDA / SPI1 RX)
                           +------------------------+
                             |    |    |    |    |
                            13   12   11   10    9
                            |    |    |    |    |
                            |    |    |    |    +-- (UART1 RTS / I2C0 SCL / SPI1 CS)
                            |    |    |    +------- (UART1 TX / I2C1 SDA / SPI1 SCK)
                            |    |    +------------ (UART1 RX / I2C1 SCL / SPI1 TX)
                            |    +----------------- (UART0 TX / I2C0 SDA / SPI1 RX)
                            +---------------------- (UART0 RX / I2C0 SCL / SPI1 CS)
```

---

## Pin Mapping Table

### Right Edge (Top to Bottom: 0 to 8)

| Silk Label | GPIO | Default | UART | I2C | SPI | PWM | Analog |
| :---: | :---: | :--- | :--- | :--- | :--- | :--- | :---: |
| **0** | `GP0` | GPIO | **UART0 TX** | **I2C0 SDA** | SPI0 RX | PWM0 A | — |
| **1** | `GP1` | GPIO | **UART0 RX** | **I2C0 SCL** | SPI0 CSn | PWM0 B | — |
| **2** | `GP2` | GPIO | **UART1 TX** | **I2C1 SDA** | SPI0 SCK | PWM1 A | — |
| **3** | `GP3` | GPIO | **UART1 RX** | **I2C1 SCL** | SPI0 TX | PWM1 B | — |
| **4** | `GP4` | GPIO | UART1 CTS | **I2C0 SDA** | SPI0 RX | PWM2 A | — |
| **5** | `GP5` | GPIO | UART1 RTS | **I2C0 SCL** | SPI0 CSn | PWM2 B | — |
| **6** | `GP6` | GPIO | **UART1 TX** | **I2C1 SDA** | SPI0 SCK | PWM3 A | — |
| **7** | `GP7` | GPIO | **UART1 RX** | **I2C1 SCL** | SPI0 TX | PWM3 B | — |
| **8** | `GP8` | GPIO | UART1 CTS | **I2C0 SDA** | **SPI1 RX** | PWM4 A | — |

---

### Bottom Edge (Right to Left: 9 to 13)

| Silk Label | GPIO | Default | UART | I2C | SPI | PWM | Analog |
| :---: | :---: | :--- | :--- | :--- | :--- | :--- | :---: |
| **9** | `GP9` | GPIO | UART1 RTS | **I2C0 SCL** | **SPI1 CSn** | PWM4 B | — |
| **10**| `GP10`| GPIO | **UART1 TX** | **I2C1 SDA** | **SPI1 SCK** | PWM5 A | — |
| **11**| `GP11`| GPIO | **UART1 RX** | **I2C1 SCL** | **SPI1 TX** | PWM5 B | — |
| **12**| `GP12`| GPIO | **UART0 TX** | **I2C0 SDA** | **SPI1 RX** | PWM6 A | — |
| **13**| `GP13`| GPIO | **UART0 RX** | **I2C0 SCL** | **SPI1 CSn** | PWM6 B | — |

---

### Left Edge (Bottom to Top: 14 to 5V)

| Silk Label | GPIO | Default | UART | I2C | SPI | PWM | Analog |
| :---: | :---: | :--- | :--- | :--- | :--- | :--- | :---: |
| **14**| `GP14`| GPIO | UART0 CTS | **I2C1 SDA** | **SPI1 SCK** | PWM7 A | — |
| **15**| `GP15`| GPIO | UART0 RTS | **I2C1 SCL** | **SPI1 TX** | PWM7 B | — |
| **26**| `GP26`| ADC0 | UART1 CTS | **I2C1 SDA** | SPI1 SCK | PWM5 A | **ADC0** |
| **27**| `GP27`| ADC1 | UART1 RTS | **I2C1 SCL** | SPI1 TX | PWM5 B | **ADC1** |
| **28**| `GP28`| ADC2 | UART0 CTS | **I2C0 SDA** | SPI0 SCK | PWM6 A | **ADC2** |
| **29**| `GP29`| ADC3 | UART0 RTS | **I2C0 SCL** | SPI0 TX | PWM6 B | **ADC3** |
| **3V3**| — | Power | — | — | — | — | 3.3V Output (up to 500mA) |
| **GND**| — | Ground| — | — | — | — | Common Ground |
| **5V** | — | Power | — | — | — | — | 5V Power Input / USB VBUS |

---

## Peripheral Mapping Summary

### Hardware I2C Channels

- **I2C0**:
  - **SDA**: `GP0`, `GP4`, `GP8`, `GP12`, `GP28`
  - **SCL**: `GP1`, `GP5`, `GP9`, `GP13`, `GP29`
- **I2C1**:
  - **SDA**: `GP2`, `GP6`, `GP10`, `GP14`, `GP26`
  - **SCL**: `GP3`, `GP7`, `GP11`, `GP15`, `GP27`

### Hardware SPI Channels

- **SPI0**:
  - **SCK**: `GP2`, `GP6`, `GP28`
  - **MOSI (TX)**: `GP3`, `GP7`, `GP29`
  - **MISO (RX)**: `GP0`, `GP4`
  - **CSn**: `GP1`, `GP5`
- **SPI1**:
  - **SCK**: `GP10`, `GP14`, `GP26`
  - **MOSI (TX)**: `GP11`, `GP15`, `GP27`
  - **MISO (RX)**: `GP8`, `GP12`
  - **CSn**: `GP9`, `GP13`

### Hardware UART Ports

- **UART0**:
  - **TX**: `GP0`, `GP12`
  - **RX**: `GP1`, `GP13`
  - **CTS**: `GP14`, `GP28`
  - **RTS**: `GP15`, `GP29`
- **UART1**:
  - **TX**: `GP2`, `GP6`, `GP10`
  - **RX**: `GP3`, `GP7`, `GP11`
  - **CTS**: `GP4`, `GP8`, `GP26`
  - **RTS**: `GP5`, `GP9`, `GP27`

### Analog Inputs (ADC)

- **ADC0**: `GP26`
- **ADC1**: `GP27`
- **ADC2**: `GP28`
- **ADC3**: `GP29`

---

## On-Board Components

| Component | Pin / GPIO | Description |
| :--- | :--- | :--- |
| **WS2812 RGB LED** | `GP16` | Onboard addressable NeoPixel RGB LED |
| **BOOT Button** | `QSPI_CS` / HW Boot | Hold down while connecting USB or pressing Reset to enter UF2 bootloader mode |
| **RESET Button** | `RUN` Pin | Hardware reset button |

---

## CircuitPython Examples

### 1. RGB NeoPixel LED

```python
import board
import neopixel
import time

pixel = neopixel.NeoPixel(board.GP16, 1, brightness=0.2)

while True:
    pixel[0] = (255, 0, 0)    # Red
    time.sleep(0.5)
    pixel[0] = (0, 255, 0)    # Green
    time.sleep(0.5)
    pixel[0] = (0, 0, 255)    # Blue
    time.sleep(0.5)
```

### 2. Default Hardware I2C (using GP0 SDA, GP1 SCL)

```python
import board
import busio

i2c = busio.I2C(scl=board.GP1, sda=board.GP0)
```

### 3. Hardware SPI (using GP10 SCK, GP11 MOSI, GP8 MISO)

```python
import board
import busio

spi = busio.SPI(clock=board.GP10, MOSI=board.GP11, MISO=board.GP8)
```
