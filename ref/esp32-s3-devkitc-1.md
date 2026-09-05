# ESP32-S3-DevKitC-1 (Dual USB-C) Reference & Pinout

A standard 44-pin (2×22 pin headers) ESP32-S3 development board (including official Espressif **ESP32-S3-DevKitC-1** and compatible boards such as **YD-ESP32-S3** and **NodeMCU-ESP32-S3**) equipped with the **ESP32-S3-WROOM-1** or **ESP32-S3-WROOM-2** module (Reference image: [Adafruit 5364-03](https://cdn-shop.adafruit.com/970x728/5364-03.jpg)).

Features a dual-core 32-bit Xtensa LX7 processor (up to 240 MHz), vector instructions for AI acceleration, 512KB SRAM, 384KB ROM, Wi-Fi 4 (802.11 b/g/n), Bluetooth 5.0 (BLE), dual USB Type-C connectors (`USB` for Native USB OTG and `UART` for USB-to-UART bridge), and an addressable RGB LED (`RGB@IO48` on v1.0, `RGB@IO38` on v1.1).

---

## ASCII Pinout Diagrams

### 1. Horizontal View (Matching Physical Board & [Adafruit Photo](https://cdn-shop.adafruit.com/970x728/5364-03.jpg))

> [!NOTE]
> **USB Port Differences from Reference Photo**:
> In the [Adafruit reference photo](https://cdn-shop.adafruit.com/970x728/5364-03.jpg) (official Espressif DevKitC-1), the top port is labeled `USB` and the bottom is `UART`.
> In the diagrams below, the USB port labels are **flipped** (top is `UART`, bottom is `USB`) to match this board variant (common on third-party boards such as YD-ESP32-S3 and NodeMCU-S3). Always check the silkscreen printed on your specific board.


```text
       +---------------------------------------------------------------------------------------------------------+
       | [Row A: Top Header - Near UART / RESET]                                                                 |
       |  G  TX  RX   1   2  42  41  40  39  38  37  36  35   0  45  48  47  21  20  19   G   G                     |
       | [•] [•] [•] [•] [•] [•] [•] [•] [•] [•] [•] [•] [•] [•] [•] [•] [•] [•] [•] [•] [•] [•]                    |
+------+                                                                                    +------------------+ |
|      |  +--------------------+         +--------+                                         | [RESET]  [ UART] | |
|      |  |                    |         |  LDO   |                     +---------+         | Button   (Bridge)| |
| PCB  |  |  ESP32-S3-WROOM    |         | 3.3V   |   [RGB LED]         | CP2102N |         +------------------+ |
| ANT  |  |  (Wi-Fi + BLE)     |         +--------+  (IO48/IO38)        | Bridge  |         +------------------+ |
|      |  |                    |                                        +---------+         | [ BOOT]  [ USB ] | |
|      |  +--------------------+                                                            | Button   (Native)| |
+------+                                                                                    +------------------+ |
       | [•] [•] [•] [•] [•] [•] [•] [•] [•] [•] [•] [•] [•] [•] [•] [•] [•] [•] [•] [•] [•] [•]                    |
       | 3V3 3V3 RST  4   5   6   7  15  16  17  18   8   3  46   9  10  11  12  13  14  5V   G                     |
       | [Row B: Bottom Header - Near USB / BOOT]                                                                |
       +---------------------------------------------------------------------------------------------------------+
```

---

### 2. Vertical View (USB-C Connectors Facing Top)

```text
                               +-----------------------------+
                               |  [ USB-C:UART ] [ USB-C:USB ]|
                               |  (Bridge COM)   (Native OTG)|
            [RESET / EN] ( )   |                             |   ( ) [BOOT / IO0]
            Button             |     [ CP2102N Bridge ]      |   Button
                               |                             |
                               |         [RGB LED]           |
                               |       (IO48 / IO38)         |
                               |                             |
                               |     +-----------------+     |
                               |     |  ESP32-S3-WROOM |     |
                               |     |  Wi-Fi + BLE    |     |
                               |     +-----------------+     |
                               |                             |
              (Common Ground)  G -| [ 1]                 [ 1] |- G   (Common Ground)
              (Common Ground)  G -| [ 2]                 [ 2] |- 5V  (5V Power Input / VBUS)
     (USB D- / Native USB)  IO19 -| [ 3]                 [ 3] |- 14  (GPIO14 / ADC2_CH3 / FSPIWP)
     (USB D+ / Native USB)  IO20 -| [ 4]     Row A       [ 4] |- 13  (GPIO13 / ADC2_CH2 / FSPIMISO)
             (General GPIO) IO21 -| [ 5]   (Left Side)   [ 5] |- 12  (GPIO12 / ADC2_CH1 / FSPISCK)
             (General GPIO) IO47 -| [ 6]                 [ 6] |- 11  (GPIO11 / ADC2_CH0 / FSPIMOSI)
        (RGB LED v1.0 / IO) IO48 -| [ 7]     Row B       [ 7] |- 10  (GPIO10 / ADC1_CH9 / FSPICS0)
       (Strapping / VDD_SPI)IO45 -| [ 8]  (Right Side)   [ 8] |- 9   (GPIO9  / ADC1_CH8 / Touch 9)
     (BOOT Button / Strap)   IO0 -| [ 9]                 [ 9] |- 46  (GPIO46 / Input Only / Strap)
      (Octal PSRAM / Flash) IO35 -| [10]                 [10] |- 3   (GPIO3  / ADC1_CH2 / Touch 3)
      (Octal PSRAM / Flash) IO36 -| [11]                 [11] |- 8   (GPIO8  / ADC1_CH7 / Touch 8)
      (Octal PSRAM / Flash) IO37 -| [12]                 [12] |- 18  (GPIO18 / ADC2_CH7 / U1RXD)
        (RGB LED v1.1 / IO) IO38 -| [13]                 [13] |- 17  (GPIO17 / ADC2_CH6 / U1TXD)
              (JTAG / GPIO) IO39 -| [14]                 [14] |- 16  (GPIO16 / ADC2_CH5 / U0CTS)
              (JTAG / GPIO) IO40 -| [15]                 [15] |- 15  (GPIO15 / ADC2_CH4 / U0RTS)
              (JTAG / GPIO) IO41 -| [16]                 [16] |- 7   (GPIO7  / ADC1_CH6 / Touch 7)
              (JTAG / GPIO) IO42 -| [17]                 [17] |- 6   (GPIO6  / ADC1_CH5 / Touch 6)
     (Touch 2 / ADC1_CH1)    IO2 -| [18]                 [18] |- 5   (GPIO5  / ADC1_CH4 / Touch 5)
     (Touch 1 / ADC1_CH0)    IO1 -| [19]                 [19] |- 4   (GPIO4  / ADC1_CH3 / Touch 4)
      (UART0 RX / Bridge)     RX -| [20]                 [20] |- RST (EN / Hardware Reset)
      (UART0 TX / Bridge)     TX -| [21]                 [21] |- 3V3 (3.3V Power Out)
              (Common Ground)  G -| [22]                 [22] |- 3V3 (3.3V Power Out)
                               +-----------------------------+
                                         [ Antenna ]
```

---

## Pin Mapping Tables

### Row A: Top Header (Near `UART` & `RESET` Buttons)
*Schematic Header J3. Listed from Left (Antenna end) to Right (USB end).*

| Pos (Antenna $\to$ USB) | Silk Label | ESP32-S3 GPIO | CircuitPython (`board.*`) | Analog / ADC | Default Functions / Peripherals | Notes |
| :---: | :---: | :---: | :---: | :---: | :--- | :--- |
| **1** (Far Left) | **G** | — | — | — | Common Ground | System ground |
| **2** | **TX** | `GPIO43` | `board.TX` / `board.IO43` | — | **UART0 TX** / CLK_OUT1 | Connected to CP2102N bridge TX |
| **3** | **RX** | `GPIO44` | `board.RX` / `board.IO44` | — | **UART0 RX** / CLK_OUT2 | Connected to CP2102N bridge RX |
| **4** | **1** | `GPIO1` | `board.IO1` | `ADC1_CH0` | Touch 1 / RTC GPIO 1 | Free GPIO / Analog input |
| **5** | **2** | `GPIO2` | `board.IO2` | `ADC1_CH1` | Touch 2 / RTC GPIO 2 | Free GPIO / Strapping pin |
| **6** | **42** | `GPIO42` | `board.IO42` | — | JTAG MTMS | Free GPIO |
| **7** | **41** | `GPIO41` | `board.IO41` | — | JTAG MTDI / CLK_OUT1 | Free GPIO |
| **8** | **40** | `GPIO40` | `board.IO40` | — | JTAG MTDO / CLK_OUT2 | Free GPIO |
| **9** | **39** | `GPIO39` | `board.IO39` | — | JTAG MTCK / CLK_OUT3 | Free GPIO |
| **10**| **38** | `GPIO38` | `board.IO38` | — | **RGB LED** (v1.1) / GPIO | WS2812 NeoPixel data on v1.1 boards |
| **11**| **37** | `GPIO37` | `board.IO37` | — | Octal PSRAM/Flash SPIDQS | **Do not use** on modules with Octal PSRAM |
| **12**| **36** | `GPIO36` | `board.IO36` | — | Octal PSRAM/Flash SPIIO7 | **Do not use** on modules with Octal PSRAM |
| **13**| **35** | `GPIO35` | `board.IO35` | — | Octal PSRAM/Flash SPIIO6 | **Do not use** on modules with Octal PSRAM |
| **14**| **0** | `GPIO0` | `board.IO0` / `board.BOOT`| — | **BOOT Button** / RTC GPIO 0 | Pulled high; active LOW (strapping pin) |
| **15**| **45** | `GPIO45` | `board.IO45` | — | Strapping Pin (VDD_SPI) | Pull-down recommended during boot |
| **16**| **48** | `GPIO48` | `board.IO48` | — | **RGB LED** (v1.0) / GPIO | WS2812 NeoPixel on v1.0 boards (`RGB@IO48`) |
| **17**| **47** | `GPIO47` | `board.IO47` | — | General GPIO / SPICLK_P | Free GPIO |
| **18**| **21** | `GPIO21` | `board.IO21` | — | General GPIO / RTC GPIO 21 | Free GPIO |
| **19**| **20** | `GPIO20` | `board.IO20` | `ADC2_CH9` | **USB D+** / UART1 CTS | Connected to Native USB-C connector |
| **20**| **19** | `GPIO19` | `board.IO19` | `ADC2_CH8` | **USB D-** / UART1 RTS | Connected to Native USB-C connector |
| **21**| **G** | — | — | — | Common Ground | System ground |
| **22** (Far Right)| **G** | — | — | — | Common Ground | System ground (near `RESET` / `UART`) |

---

### Row B: Bottom Header (Near `USB` & `BOOT` Buttons)
*Schematic Header J1. Listed from Left (Antenna end) to Right (USB end).*

| Pos (Antenna $\to$ USB) | Silk Label | ESP32-S3 GPIO | CircuitPython (`board.*`) | Analog / ADC | Default Functions / Peripherals | Notes |
| :---: | :---: | :---: | :---: | :---: | :--- | :--- |
| **1** (Far Left) | **3V3** | — | — | — | 3.3V Regulated Power Rail | Power supply output (or regulated 3.3V input) |
| **2** | **3V3** | — | — | — | 3.3V Regulated Power Rail | Tied directly to Pin 1 |
| **3** | **RST** | `EN` / `CHIP_PU` | — | — | Reset / Enable Line | Pulled high; pull LOW to reset chip |
| **4** | **4** | `GPIO4` | `board.IO4` | `ADC1_CH3` | Touch 4 / RTC GPIO 4 | Free GPIO |
| **5** | **5** | `GPIO5` | `board.IO5` | `ADC1_CH4` | Touch 5 / RTC GPIO 5 | Free GPIO (Hardware I2C SDA) |
| **6** | **6** | `GPIO6` | `board.IO6` | `ADC1_CH5` | Touch 6 / RTC GPIO 6 | Free GPIO (Hardware I2C SCL) |
| **7** | **7** | `GPIO7` | `board.IO7` | `ADC1_CH6` | Touch 7 / RTC GPIO 7 | Free GPIO (Hardware SPI SCK) |
| **8** | **15** | `GPIO15` | `board.IO15` | `ADC2_CH4` | UART0 RTS / RTC GPIO 15 | 32.768 kHz XTAL P |
| **9** | **16** | `GPIO16` | `board.IO16` | `ADC2_CH5` | UART0 CTS / RTC GPIO 16 | 32.768 kHz XTAL N |
| **10**| **17** | `GPIO17` | `board.IO17` | `ADC2_CH6` | UART1 TXD / RTC GPIO 17 | Free GPIO |
| **11**| **18** | `GPIO18` | `board.IO18` | `ADC2_CH7` | UART1 RXD / RTC GPIO 18 | Free GPIO |
| **12**| **8** | `GPIO8` | `board.IO8` | `ADC1_CH7` | Touch 8 / RTC GPIO 8 | Free GPIO (Hardware SPI MISO) |
| **13**| **3** | `GPIO3` | `board.IO3` | `ADC1_CH2` | Touch 3 / RTC GPIO 3 | Free GPIO |
| **14**| **46** | `GPIO46` | `board.IO46` | — | Digital Input Only / Strap | **Input only**; no output driver. Strapping pin |
| **15**| **9** | `GPIO9` | `board.IO9` | `ADC1_CH8` | Touch 9 / FSPIHD | Free GPIO (Hardware SPI MOSI) |
| **16**| **10** | `GPIO10` | `board.IO10` | `ADC1_CH9` | Touch 10 / **FSPI CS0** | Standard Hardware SPI CS |
| **17**| **11** | `GPIO11` | `board.IO11` | `ADC2_CH0` | Touch 11 / **FSPI MOSI** | Standard Hardware SPI MOSI |
| **18**| **12** | `GPIO12` | `board.IO12` | `ADC2_CH1` | Touch 12 / **FSPI SCK** | Standard Hardware SPI SCK |
| **19**| **13** | `GPIO13` | `board.IO13` | `ADC2_CH2` | Touch 13 / **FSPI MISO** | Standard Hardware SPI MISO |
| **20**| **14** | `GPIO14` | `board.IO14` | `ADC2_CH3` | Touch 14 / FSPIWP | Standard Hardware SPI WP |
| **21**| **5V** | — | — | — | 5V Power Input / VBUS | Direct connection to USB 5V rail |
| **22** (Far Right)| **G** | — | — | — | Common Ground | System ground (near `BOOT` / `USB`) |

---

## On-Board Components & Dual USB-C Ports

> [!NOTE]
> **USB Port Labeling on Board Variants**:
> On official Espressif DevKitC-1 v1.0/v1.1 boards ([photo](https://cdn-shop.adafruit.com/970x728/5364-03.jpg)), the Row A port is labeled `USB` and Row B is `UART`. On many third-party boards (e.g., YD-ESP32-S3, NodeMCU-S3), the silkscreen labels are flipped: Row A is labeled `UART` (Bridge) and Row B is labeled `USB` (Native). Always check the silkscreen labels printed beside each port on your board.

### Dual USB-C Functions
1. **`UART` Port (USB-to-UART Bridge)**:
   - Located on the **Row A** side (near `RESET` button).
   - Connected via an onboard CP2102N / CH343 / CH340 bridge chip to **`GPIO43` (TX)** and **`GPIO44` (RX)**.
   - Primary port for firmware flashing and serial monitoring (`idf.py monitor`).
2. **`USB` Port (Native USB OTG)**:
   - Located on the **Row B** side (near `BOOT` button).
   - Wired directly to the internal ESP32-S3 USB PHY via **`GPIO19` (D-)** and **`GPIO20` (D+)**.
   - Supports native USB HID (gamepad, keyboard, mouse), USB CDC (serial console), USB MSC (disk drive), and USB-Serial-JTAG debugging.

### Buttons & Status LEDs
| Component | Physical Position | Connected To | Description |
| :--- | :--- | :--- | :--- |
| **RESET Button** | Near `UART` port (Row A) | `EN` / `CHIP_PU` | Hardware reset line. Press to reboot the microcontroller. |
| **BOOT Button** | Near `USB` port (Row B) | `GPIO0` | Active LOW. Hold while resetting to enter ROM bootloader mode. |
| **Power LED** | Center PCB | 3.3V Rail | Red LED indicates 3.3V power is present. |
| **Addressable RGB LED** | Center PCB | `GPIO48` (v1.0) or `GPIO38` (v1.1) | WS2812B NeoPixel (`RGB@IO48` silkscreen on v1.0). |

---

## Important Hardware & Pin Usage Cautions

1. **Octal PSRAM / Flash Pins (`GPIO35`, `GPIO36`, `GPIO37` on Row A)**:
   - On modules with 8MB Octal PSRAM (e.g. `WROOM-1-N8R8`, `N16R8`, `WROOM-2`), these pins are tied internally to the high-speed PSRAM bus. **Do not connect external circuits to GPIO35, GPIO36, or GPIO37**, or the board will fail to boot.
2. **Input-Only Pin (`GPIO46` on Row B)**:
   - `GPIO46` is purely a digital input. It does not have an output driver and cannot drive external outputs.
3. **Native USB Pins (`GPIO19`, `GPIO20` on Row A)**:
   - If using the native `USB` Type-C port, leave `GPIO19` and `GPIO20` disconnected on the header.
4. **ADC2 & Wi-Fi Coexistence**:
   - ADC2 pins (`GPIO11`–`GPIO20`) share analog hardware with Wi-Fi. When Wi-Fi is active, use **ADC1** pins (`GPIO1`–`GPIO10`, `GPIO3`, `GPIO4`) for analog readings.

---

## Wiring USR-ES1 (W5500) Ethernet to ESP32-S3-DevKitC-1

### Standard Hardware SPI (FSPI) Option
Using the dedicated SPI pins on **Row B**:

| USR-ES1 (W5500) Pin | Signal | DevKitC-1 Header | Silk Label | ESP32-S3 GPIO | Description |
| :---: | :---: | :---: | :---: | :---: | :--- |
| **J1-1 / J2-1** | **GND** | Row B, Pin 22 (or Row A, Pin 1/21/22) | `G` | — | System ground |
| **J2-2 / J2-3** | **3.3V** | Row B, Pin 1 or 2 | `3V3` | — | 3.3V power rail |
| **J1-4** | **SCLK** | Row B, Pin 18 | `12` | `GPIO12` | Hardware SPI Clock (`FSPI CLK`) |
| **J1-3** | **MOSI** | Row B, Pin 17 | `11` | `GPIO11` | Hardware SPI MOSI (`FSPI D`) |
| **J2-6** | **MISO** | Row B, Pin 19 | `13` | `GPIO13` | Hardware SPI MISO (`FSPI Q`) |
| **J1-5** | **SCSn** | Row B, Pin 16 | `10` | `GPIO10` | Hardware SPI CS (`FSPI CS0`) |
| **J2-5** | **RSTn** | Row B, Pin 15 | `9` | `GPIO9` | Hardware Reset (active LOW) |
| **J1-6** | **INTn** | Row B, Pin 13 | `3` | `GPIO3` | *(Optional)* Hardware interrupt |
| **J2-4** | **PWDN** | — | — | — | Leave disconnected (N.C.) |

---

### Universal Portability Pinout (Compatible with Seeed Studio XIAO ESP32-S3)
Using pins shared by both the Seeed Studio XIAO and the DevKitC-1:

| Function | Signal | ESP32-S3 GPIO | DevKitC-1 Header & Silk | Seeed Studio XIAO Pin |
| :--- | :--- | :---: | :---: | :---: |
| **I2C Bus** | **SDA** | `GPIO5` | Row B, silk `5` | **D4** (Pin 5) |
| **I2C Bus** | **SCL** | `GPIO6` | Row B, silk `6` | **D5** (Pin 6) |
| **W5500 SPI**| **SCLK** | `GPIO7` | Row B, silk `7` | **D8** (Pin 9) |
| **W5500 SPI**| **MISO** | `GPIO8` | Row B, silk `8` | **D9** (Pin 10) |
| **W5500 SPI**| **MOSI** | `GPIO9` | Row B, silk `9` | **D10** (Pin 11) |
| **W5500 Control** | **SCSn** (CS) | `GPIO4` | Row B, silk `4` | **D3** (Pin 4) |
| **W5500 Control** | **RSTn** | `GPIO3` | Row B, silk `3` | **D2** (Pin 3) |
| **W5500 Control** | **INTn** | `GPIO1` | Row A, silk `1` | **D0** (Pin 1) |
| **Power** | **3.3V** | `3V3` | Row B, silk `3V3` | Pin 12 (`3V3`) |
| **Ground** | **GND** | `GND` | Row A or B, silk `G` | Pin 13 (`GND`) |
