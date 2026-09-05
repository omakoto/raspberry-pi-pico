# USR-ES1 (W5500) Ethernet Module Reference & Pinout

A compact, breadboard-compatible SPI-to-LAN Ethernet converter module based on the **WIZnet W5500** hardwired TCP/IP embedded Ethernet controller (e.g. Amazon ASIN [B09LM6BWVN](https://www.amazon.com/dp/B09LM6BWVN)).

Features an integrated RJ45 jack with magnetics and indicator LEDs, hardwired TCP/IP stack (supporting TCP, UDP, IPv4, ICMP, ARP, IGMP, PPPoE), 32KB internal buffer, and an 80 MHz SPI slave interface exposed across two 1×6 pin headers (2.54 mm / 0.1" pitch, 20.32 mm / 0.8" row spacing).

---

## ASCII Pinout Diagram

### Top View (RJ45 Facing Up)

```text
                            +-----------------------------+
                            |                             |
                            |      [ RJ45 ETHERNET ]      |
                            |       (Link/Act LEDs)       |
                            |                             |
                         +--+-----------------------------+--+
                         |                                   |
(System Ground)     GND -| [J1-1]                     [J2-1] |- GND        (System Ground)
(System Ground)     GND -| [J1-2]                     [J2-2] |- 3.3V       (3.3V Power Input)
(SPI MOSI / In)    MOSI -| [J1-3]        USR-ES1      [J2-3] |- 3.3V       (3.3V Power Input)
(SPI Clock)        SCLK -| [J1-4]      W5500 Module   [J2-4] |- NC / PWDN  (Power-Down / N.C.)
(SPI CS / Select)  SCSn -| [J1-5]       (Top View)    [J2-5] |- RSTn       (Reset, Active LOW)
(Interrupt Out)    INTn -| [J1-6]                     [J2-6] |- MISO       (SPI MISO / Out)
                         |                                   |
                         +-----------------------------------+
```

### Bottom View / Pin Header Footprint (Looking at Component/Pin Side)

```text
                         +-----------------------------------+
                         |                                   |
(System Ground)     GND -| [J2-1]                     [J1-1] |- GND        (System Ground)
(3.3V Power Input) 3.3V -| [J2-2]      +---------+    [J1-2] |- GND        (System Ground)
(3.3V Power Input) 3.3V -| [J2-3]      |  W5500  |    [J1-3] |- MOSI       (SPI MOSI / In)
(Power-Down/N.C.)  PWDN -| [J2-4]      |   IC    |    [J1-4] |- SCLK       (SPI Clock)
(Reset, Active LOW)RSTn -| [J2-5]      +---------+    [J1-5] |- SCSn       (SPI CS / Select)
(SPI MISO / Out)   MISO -| [J2-6]                     [J1-6] |- INTn       (Interrupt Out)
                         |                                   |
                         +--+-----------------------------+--+
                            |                             |
                            |      [ RJ45 CONNECTOR ]     |
                            +-----------------------------+
```

---

## Pin Mapping Tables

### Header J1 (Left Edge in Top View)

| Pin # | Silk Label | Signal Type | Direction | Description |
| :---: | :---: | :---: | :---: | :--- |
| **J1-1** | **GND** | Power | — | Common system ground |
| **J1-2** | **GND** | Power | — | Common system ground (internally tied to J1-1, J2-1) |
| **J1-3** | **MOSI** | SPI Data | Input | **SPI Master-Out-Slave-In**: Receives data from MCU |
| **J1-4** | **SCLK** | SPI Clock | Input | **SPI Serial Clock**: Driven by MCU (up to 80 MHz) |
| **J1-5** | **SCSn** | SPI Select | Input | **SPI Slave Select (CS)**: Active LOW, driven by MCU |
| **J1-6** | **INTn** | Interrupt | Output | **Interrupt**: Active LOW; indicates packet receipt / socket event |

---

### Header J2 (Right Edge in Top View)

| Pin # | Silk Label | Signal Type | Direction | Description |
| :---: | :---: | :---: | :---: | :--- |
| **J2-1** | **GND** | Power | — | Common system ground |
| **J2-2** | **3.3V** / **VIN** | Power | Input | **+3.3V Power Supply Input** (requires $\ge 200\text{ mA}$) |
| **J2-3** | **3.3V** / **VIN** | Power | Input | **+3.3V Power Supply Input** (internally tied to J2-2) |
| **J2-4** | **NC** / **PWDN** | Control | Input | **Power Down / No Connect**: Normally float or pull LOW for normal operation; pull HIGH to enter low-power sleep |
| **J2-5** | **RSTn** | Control | Input | **Hardware Reset**: Active LOW (hold LOW $\ge 2\,\mu\text{s}$, then wait $\ge 150\text{ ms}$ for PLL stabilization) |
| **J2-6** | **MISO** | SPI Data | Output | **SPI Master-In-Slave-Out**: Sends data to MCU |

---

## Technical & Electrical Specifications

| Parameter | Specification | Notes |
| :--- | :--- | :--- |
| **Ethernet Controller** | WIZnet W5500 | Embedded 10/100 Ethernet PHY + MAC |
| **Protocols Supported** | TCP, UDP, IPv4, ICMP, ARP, IGMP, PPPoE | Fully hardwired TCP/IP stack in silicon |
| **Simultaneous Sockets**| 8 independent hardware sockets | 32 KB internal buffer shared across sockets |
| **Operating Voltage** | **3.3V DC $\pm 5\%$** | **No onboard 5V regulator!** Do not feed 5V to 3.3V pins |
| **Operating Current** | ~130 mA (typical), 200 mA (peak) | Ensure MCU 3.3V rail can supply $\ge 200\text{ mA}$ |
| **I/O Logic Level** | 3.3V TTL (SPI inputs are 5V tolerant) | 3.3V native logic; direct connect to RP2040/RP2350 |
| **SPI Clock Speed** | Up to 80 MHz | SPI Mode 0 (CPOL=0, CPHA=0) or Mode 3 (CPOL=1, CPHA=1) |
| **Physical Dimensions** | 28.5 mm (L) × 23.0 mm (W) × 24.0 mm (H) | PCB size 25 mm × 23 mm; 2.54 mm (0.1") pin pitch |
| **Row Spacing** | 20.32 mm (0.8" / 8 standard breadboard rows) | Easily spans standard 0.3" breadboard center channel |

---

## Raspberry Pi Pico (RP2040 / RP2350) Wiring Guide

The USR-ES1 module connects to the Raspberry Pi Pico via one of the RP2040/RP2350 hardware SPI blocks (`SPI0` or `SPI1`).

### Primary Recommended Wiring (`SPI0`)

| USR-ES1 Pin | Signal Name | Raspberry Pi Pico Pin | Pico Pin # | Notes |
| :---: | :---: | :---: | :---: | :--- |
| **J2-2 / J2-3** | **3.3V** | `3V3_OUT` | **Pin 36** | 3.3V output from Pico onboard buck-boost |
| **J1-1 / J2-1** | **GND** | `GND` | **Pin 38 / Pin 23** | Common ground |
| **J1-3** | **MOSI** | `GP19` (SPI0 TX) | **Pin 25** | Pico MOSI $\rightarrow$ USR-ES1 MOSI |
| **J2-6** | **MISO** | `GP16` (SPI0 RX) | **Pin 21** | USR-ES1 MISO $\rightarrow$ Pico MISO |
| **J1-4** | **SCLK** | `GP18` (SPI0 SCK)| **Pin 24** | SPI Clock |
| **J1-5** | **SCSn** | `GP17` (SPI0 CSn)| **Pin 22** | SPI Chip Select (Active LOW) |
| **J2-5** | **RSTn** | `GP20` | **Pin 26** | Hardware Reset GPIO (Optional but recommended) |
| **J1-6** | **INTn** | `GP21` | **Pin 27** | Hardware Interrupt GPIO (Optional) |
| **J2-4** | **NC/PWDN** | Unconnected / GND | — | Leave floating or ground for normal operation |

---

### Alternative Wiring (`SPI1`)

| USR-ES1 Pin | Signal Name | Raspberry Pi Pico Pin | Pico Pin # | Notes |
| :---: | :---: | :---: | :---: | :--- |
| **J2-2 / J2-3** | **3.3V** | `3V3_OUT` | **Pin 36** | 3.3V output |
| **J1-1 / J2-1** | **GND** | `GND` | **Pin 18** | Common ground |
| **J1-3** | **MOSI** | `GP15` (SPI1 TX) | **Pin 20** | Pico MOSI $\rightarrow$ USR-ES1 MOSI |
| **J2-6** | **MISO** | `GP12` (SPI1 RX) | **Pin 16** | USR-ES1 MISO $\rightarrow$ Pico MISO |
| **J1-4** | **SCLK** | `GP14` (SPI1 SCK)| **Pin 19** | SPI Clock |
| **J1-5** | **SCSn** | `GP13` (SPI1 CSn)| **Pin 17** | SPI Chip Select (Active LOW) |
| **J2-5** | **RSTn** | `GP11` | **Pin 15** | Hardware Reset GPIO |
| **J1-6** | **INTn** | `GP10` | **Pin 14** | Hardware Interrupt GPIO |

---

## Code Examples

### CircuitPython (`adafruit_wiznet5k`)

Install `adafruit_wiznet5k` and `adafruit_bus_device` in the Pico's `lib/` directory.

```python
import board
import busio
import digitalio
from adafruit_wiznet5k.adafruit_wiznet5k import WIZNET5K
import adafruit_wiznet5k.adafruit_wiznet5k_socket as socket

# SPI Configuration for SPI0
cs = digitalio.DigitalInOut(board.GP17)
spi = busio.SPI(clock=board.GP18, MOSI=board.GP19, MISO=board.GP16)
reset = digitalio.DigitalInOut(board.GP20)

# Initialize Ethernet module
eth = WIZNET5K(spi, cs, reset=reset, is_dhcp=True)

print("Connected via DHCP!")
print("IP Address:", eth.pretty_ip(eth.ip_address))
```

### Pico C/C++ SDK

When using the Raspberry Pi Pico C/C++ SDK with the W5500 driver (e.g. `pico-w5500` or `wiznet-io/ioLibrary_Driver`):

```c
#include "pico/stdlib.h"
#include "hardware/spi.h"

#define W5500_SPI_PORT  spi0
#define PIN_MISO        16
#define PIN_CS          17
#define PIN_SCK         18
#define PIN_MOSI        19
#define PIN_RST         20

void init_w5500_hardware(void) {
    // Initialize SPI at 33 MHz (W5500 supports up to 80 MHz)
    spi_init(W5500_SPI_PORT, 33 * 1000 * 1000);
    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);

    // Chip Select is active LOW, manually driven
    gpio_init(PIN_CS);
    gpio_set_dir(PIN_CS, GPIO_OUT);
    gpio_put(PIN_CS, 1);

    // Hardware Reset Pulse: LOW for >= 2 us, wait >= 150 ms
    gpio_init(PIN_RST);
    gpio_set_dir(PIN_RST, GPIO_OUT);
    gpio_put(PIN_RST, 0);
    sleep_us(10);
    gpio_put(PIN_RST, 1);
    sleep_ms(160);
}
```

---

## Hardware & Integration Notes

1. **Power Supply Requirements**:
   - The USR-ES1 **does not include an onboard 3.3V LDO regulator**.
   - Power must be supplied directly to pin `J2-2` or `J2-3` from a stable **3.3V** supply capable of delivering at least **200 mA**.
   - Connecting `5V` or USB `VBUS` to the `3.3V` pin will destroy the W5500 controller!
   - On the Raspberry Pi Pico, `3V3_OUT` (pin 36) is generated by the onboard RT6150 / buck-boost regulator and can provide up to 300–500 mA, which is sufficient to power the USR-ES1 when no other heavy 3.3V peripherals are connected.

2. **Reset Timing Requirement**:
   - When asserting `RSTn`, pull it LOW for a minimum duration of **2 µs**.
   - After releasing `RSTn` back HIGH, the host microcontroller **must wait at least 150 ms** before issuing SPI commands to allow the W5500 internal PLL clock to fully stabilize.

3. **Power-Down (PWDN) Pin**:
   - On most USR-ES1 board batches, `J2-4` is connected to the W5500 `PWDN` pin or left unconnected (N.C.).
   - If connected, it is pulled LOW internally. Keeping it disconnected or pulling it to ground keeps the module powered on. Driving it HIGH enters power-down mode.
