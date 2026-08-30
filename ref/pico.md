# Raspberry Pi Pico Family Reference & Pinout

A comprehensive hardware and pinout reference for the four official Raspberry Pi Pico microcontroller boards:
- **Raspberry Pi Pico** (Pico 1)
- **Raspberry Pi Pico W** (Pico 1 W)
- **Raspberry Pi Pico 2**
- **Raspberry Pi Pico 2 W**

All four models share a standardized, pin-compatible 40-pin DIP form factor (21 mm × 51 mm) with castellated edges.

---

## ASCII Pinout Diagram (All Models)

```text
                                  [ Micro-USB ]
                               +-----------------+
        (UART0 TX / I2C0 SDA)  GP0 -| [1]          [40] |- VBUS  (5V USB Input)
        (UART0 RX / I2C0 SCL)  GP1 -| [2]          [39] |- VSYS  (1.8V-5.5V Main Power)
                               GND -| [3]          [38] |- GND
       (UART1 TX / I2C1 SDA)   GP2 -| [4]          [37] |- 3V3_EN (Regulator Enable)
       (UART1 RX / I2C1 SCL)   GP3 -| [5]          [36] |- 3V3_OUT (3.3V Power Out)
       (UART1 CTS / I2C0 SDA)  GP4 -| [6]          [35] |- ADC_VREF (ADC Reference)
       (UART1 RTS / I2C0 SCL)  GP5 -| [7]   PICO   [34] |- GP28 / ADC2
                               GND -| [8]  FAMILY  [33] |- AGND (ADC Ground Reference)
       (UART1 TX / I2C1 SDA)   GP6 -| [9]          [32] |- GP27 / ADC1 / I2C1 SCL
       (UART1 RX / I2C1 SCL)   GP7 -| [10]         [31] |- GP26 / ADC0 / I2C1 SDA
       (UART1 CTS / I2C0 SDA)  GP8 -| [11]         [30] |- RUN (Hardware Reset / Enable)
       (UART1 RTS / I2C0 SCL)  GP9 -| [12]         [29] |- GP22
                               GND -| [13]         [28] |- GND
       (UART1 TX / I2C1 SDA)  GP10 -| [14]         [27] |- GP21 / I2C0 SCL
       (UART1 RX / I2C1 SCL)  GP11 -| [15]         [26] |- GP20 / I2C0 SDA
       (UART0 TX / I2C0 SDA)  GP12 -| [16]         [25] |- GP19 / SPI0 TX(MOSI) / I2C1 SCL
       (UART0 RX / I2C0 SCL)  GP13 -| [17]         [24] |- GP18 / SPI0 SCK      / I2C1 SDA
                               GND -| [18]         [23] |- GND
       (UART0 CTS / I2C1 SDA) GP14 -| [19]         [22] |- GP17 / SPI0 CS
       (UART0 RTS / I2C1 SCL) GP15 -| [20]         [21] |- GP16 / SPI0 RX(MISO)
                               +-----------------+
                                     | | |
                             [ SWD Debug Port ]*
                       *(Pico / Pico 2: 3-pin bottom header)
                       *(Pico W / Pico 2 W: 3 pads near center)
```

---

## Comparison of Models & Key Differences

| Feature | Raspberry Pi Pico (1) | Raspberry Pi Pico W (1W) | Raspberry Pi Pico 2 | Raspberry Pi Pico 2 W |
| :--- | :--- | :--- | :--- | :--- |
| **SoC** | **RP2040** | **RP2040** | **RP2350** | **RP2350** |
| **CPU Architecture** | Dual-core ARM Cortex-M0+ | Dual-core ARM Cortex-M0+ | Dual ARM Cortex-M33 **OR** Dual Hazard3 RISC-V | Dual ARM Cortex-M33 **OR** Dual Hazard3 RISC-V |
| **Clock Frequency** | 133 MHz | 133 MHz | 150 MHz | 150 MHz |
| **SRAM** | 264 KB | 264 KB | **520 KB** | **520 KB** |
| **QSPI Flash** | 2 MB | 2 MB | **4 MB** | **4 MB** |
| **Wireless Module** | None | **Infineon CYW43439** | None | **Infineon CYW43439** |
| **Wi-Fi** | — | Wi-Fi 4 (802.11 b/g/n, 2.4 GHz) | — | Wi-Fi 4 (802.11 b/g/n, 2.4 GHz) |
| **Bluetooth** | — | Bluetooth 5.2 / BLE | — | Bluetooth 5.2 / BLE |
| **Antenna** | — | Onboard PCB Antenna | — | Onboard PCB Antenna |
| **Onboard LED Pin** | `GP25` (RP2040 GPIO) | `CYW43439 GPIO0` (Wireless module) | `GP25` (RP2350 GPIO) | `CYW43439 GPIO0` (Wireless module) |
| **SWD Debug Port** | 3-pin header at bottom edge | 3 test pads near board center | 3-pin header at bottom edge | 3 test pads near board center |
| **Hardware Security** | None | None | Arm TrustZone, Secure Boot, OTP, SHA-256 | Arm TrustZone, Secure Boot, OTP, SHA-256 |
| **PIO State Machines** | 2 blocks (8 state machines) | 2 blocks (8 state machines) | **3 blocks (12 state machines)** | **3 blocks (12 state machines)** |

---

## Detailed Highlighting of Key Differences

### 1. SoC & Core Architecture (Pico 1 vs Pico 2)
- **RP2040 (Pico / Pico W)**: Dual-core ARM Cortex-M0+ at 133 MHz, 264 KB SRAM, 2 MB Flash.
- **RP2350 (Pico 2 / Pico 2 W)**: Dual-core ARM Cortex-M33 with FPU / DSP instructions (or selectable dual Hazard3 RISC-V cores) at 150 MHz, 520 KB SRAM (nearly 2x), 4 MB Flash (2x), and a 3rd PIO block (12 state machines total). Adds hardware security features (TrustZone-M, encrypted boot, SHA-256 accelerator).

### 2. Onboard LED Routing (Non-W vs W Models)
- **Non-W (Pico / Pico 2)**: The green user LED is directly connected to SoC pin **`GP25`**.
- **Wireless (Pico W / Pico 2 W)**: `GP25` is NOT connected to the LED. Instead, the LED is driven by **`GPIO0` on the Infineon CYW43439 wireless chip** via the internal SPI interface.
  - *CircuitPython*: Transparently handled via `board.LED` on all 4 models.
  - *MicroPython*: Use `machine.Pin("LED", machine.Pin.OUT)` instead of `machine.Pin(25)`.
  - *C/C++ SDK*: Include `pico/cyw43_arch.h` and use `cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1)`.

### 3. SWD Debug Header Location
- **Pico & Pico 2**: Standard 3-pin 0.1" pitch SWD header (`SWCLK`, `GND`, `SWDIO`) located at the **bottom edge** of the board.
- **Pico W & Pico 2 W**: The bottom edge is occupied by the wireless metal shield and PCB antenna. The 3 SWD debug connection points are relocated to **small circular test pads near the center** of the PCB between the SoC and wireless shield.

### 4. Power & Internal Sensing
- **VBUS Detection**:
  - *Pico / Pico 2*: `GP24` is wired to a voltage divider sensing USB `VBUS` power.
  - *Pico W / Pico 2 W*: `VBUS` sense is routed to `CYW43439 WL_GPIO2`.
- **Power Save Mode (SMPS PSM)**:
  - *Pico / Pico 2*: `GP23` controls power-saving PWM/PFM mode on the voltage regulator.
  - *Pico W / Pico 2 W*: SMPS mode is controlled via `CYW43439 WL_GPIO1`.

---

## 40-Pin Header Details

| Pin # | Silk Pin Name | Default Function | Alternate Functions (I2C / SPI / UART / ADC / PWM) |
| :---: | :---: | :---: | :--- |
| **1** | **GP0** | Digital GPIO | **UART0 TX**, **I2C0 SDA**, SPI0 RX, PWM0 A |
| **2** | **GP1** | Digital GPIO | **UART0 RX**, **I2C0 SCL**, SPI0 CS, PWM0 B |
| **3** | **GND** | Ground | Common Ground |
| **4** | **GP2** | Digital GPIO | **UART1 TX**, **I2C1 SDA**, SPI0 SCK, PWM1 A |
| **5** | **GP3** | Digital GPIO | **UART1 RX**, **I2C1 SCL**, SPI0 TX, PWM1 B |
| **6** | **GP4** | Digital GPIO | **UART1 CTS**, **I2C0 SDA**, SPI0 RX, PWM2 A |
| **7** | **GP5** | Digital GPIO | **UART1 RTS**, **I2C0 SCL**, SPI0 CS, PWM2 B |
| **8** | **GND** | Ground | Common Ground |
| **9** | **GP6** | Digital GPIO | **UART1 TX**, **I2C1 SDA**, SPI0 SCK, PWM3 A |
| **10**| **GP7** | Digital GPIO | **UART1 RX**, **I2C1 SCL**, SPI0 TX, PWM3 B |
| **11**| **GP8** | Digital GPIO | **UART1 CTS**, **I2C0 SDA**, SPI1 RX, PWM4 A |
| **12**| **GP9** | Digital GPIO | **UART1 RTS**, **I2C0 SCL**, SPI1 CS, PWM4 B |
| **13**| **GND** | Ground | Common Ground |
| **14**| **GP10**| Digital GPIO | **UART1 TX**, **I2C1 SDA**, SPI1 SCK, PWM5 A |
| **15**| **GP11**| Digital GPIO | **UART1 RX**, **I2C1 SCL**, SPI1 TX, PWM5 B |
| **16**| **GP12**| Digital GPIO | **UART0 TX**, **I2C0 SDA**, SPI1 RX, PWM6 A |
| **17**| **GP13**| Digital GPIO | **UART0 RX**, **I2C0 SCL**, SPI1 CS, PWM6 B |
| **18**| **GND** | Ground | Common Ground |
| **19**| **GP14**| Digital GPIO | **UART0 CTS**, **I2C1 SDA**, SPI1 SCK, PWM7 A |
| **20**| **GP15**| Digital GPIO | **UART0 RTS**, **I2C1 SCL**, SPI1 TX, PWM7 B |
| **21**| **GP16**| Digital GPIO | **SPI0 RX (MISO)**, UART0 TX, I2C0 SDA, PWM0 A |
| **22**| **GP17**| Digital GPIO | **SPI0 CS**, UART0 RX, I2C0 SCL, PWM0 B |
| **23**| **GND** | Ground | Common Ground |
| **24**| **GP18**| Digital GPIO | **SPI0 SCK**, UART0 CTS, I2C1 SDA, PWM1 A |
| **25**| **GP19**| Digital GPIO | **SPI0 TX (MOSI)**, UART0 RTS, I2C1 SCL, PWM1 B |
| **26**| **GP20**| Digital GPIO | **I2C0 SDA**, UART1 TX, PWM2 A |
| **27**| **GP21**| Digital GPIO | **I2C0 SCL**, UART1 RX, PWM2 B |
| **28**| **GND** | Ground | Common Ground |
| **29**| **GP22**| Digital GPIO | UART1 CTS, PWM3 A |
| **30**| **RUN** | Reset | Active LOW hardware reset (pull to GND to restart) |
| **31**| **GP26**| ADC0 / GPIO | **ADC Channel 0**, I2C1 SDA, UART1 RTS, PWM5 A |
| **32**| **GP27**| ADC1 / GPIO | **ADC Channel 1**, I2C1 SCL, UART1 TX, PWM5 B |
| **33**| **AGND**| Analog Ground | Low-noise ground reference for ADC |
| **34**| **GP28**| ADC2 / GPIO | **ADC Channel 2**, UART1 RX, PWM6 A |
| **35**| **ADC_VREF**| ADC Voltage Ref| 3.3V Analog reference voltage input/output |
| **36**| **3V3** | Power Output | 3.3V DC power output from onboard switching regulator |
| **37**| **3V3_EN**| Power Enable | Pull to GND to shut down onboard 3.3V regulator |
| **38**| **GND** | Ground | Common Ground |
| **39**| **VSYS**| System Power | **1.8V to 5.5V** power input (battery / external power) |
| **40**| **VBUS**| USB 5V | 5V power from micro-USB port (or input when host) |

---

## Powering the Board

### External Power (Battery / Power Supply)

```text
       [ USB PORT ]
 (Pin 1)  [        ]  (Pin 40) VBUS
 (Pin 2)  [  PICO  ]  (Pin 39) VSYS  <--- +5V / Battery In (+1.8V to +5.5V)
 (Pin 3)  [        ]  (Pin 38) GND   <--- Ground (GND)
```

- **Connect +5V / Battery (+) to `VSYS` (Pin 39)**:
  - An onboard Schottky diode sits between `VBUS` (Pin 40) and `VSYS` (Pin 39).
  - Feeding external power to `VSYS` prevents reverse current back into your PC's USB port when connected simultaneously.
- **Connect GND (-) to `GND` (Pin 38)**.
