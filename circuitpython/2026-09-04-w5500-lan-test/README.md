# USR-ES1 (W5500) Ethernet TCP Server Test

A CircuitPython application running a TCP echo server on port `10110` over wired Ethernet using a **USR-ES1** (WIZnet W5500) module connected to a **Raspberry Pi Pico** (RP2040 / RP2350).

---

## Hardware Wiring

Connect the USR-ES1 module to the Raspberry Pi Pico using the recommended hardware `SPI0` pinout from [`ref/usr-es1-w5500.md`](file:///home/omakoto/cbin/src/raspberry-pi-pico/ref/usr-es1-w5500.md):

| USR-ES1 Pin | Silk Label | Raspberry Pi Pico Pin | Pico Pin # | Description |
| :---: | :---: | :---: | :---: | :--- |
| **J2-2 / J2-3** | **3.3V** | `3V3_OUT` | **Pin 36** | 3.3V Power input (requires $\ge 200\text{ mA}$) |
| **J1-1 / J2-1** | **GND** | `GND` | **Pin 38 / Pin 23**| System Ground |
| **J1-3** | **MOSI** | `GP19` (SPI0 TX) | **Pin 25** | SPI Data In (Pico MOSI $\rightarrow$ W5500 MOSI) |
| **J2-6** | **MISO** | `GP16` (SPI0 RX) | **Pin 21** | SPI Data Out (W5500 MISO $\rightarrow$ Pico MISO) |
| **J1-4** | **SCLK** | `GP18` (SPI0 SCK)| **Pin 24** | SPI Clock |
| **J1-5** | **SCSn** | `GP17` (SPI0 CSn)| **Pin 22** | SPI Chip Select (Active LOW) |
| **J2-5** | **RSTn** | `GP20` | **Pin 26** | Hardware Reset (Active LOW) |
| **J2-4** | **NC/PWDN**| Floating / Ground| — | Leave unconnected or tied to GND for normal operation |

> [!CAUTION]
> The USR-ES1 module requires a **3.3V** power supply and does **NOT** feature an onboard 5V regulator. Never connect 5V or USB VBUS to the 3.3V pins.

---

## Configuration (`config.toml`)

The application parses configuration settings from [`config.toml`](file:///home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/2026-09-04-w5500-lan-test/config.toml) (overridden by `config-base.toml` if present):

```toml
hostname = "w5500-test"
tcp_port = 10110

# Optional SPI pin overrides:
# spi_sck = 18
# spi_mosi = 19
# spi_miso = 16
# spi_cs = 17
# spi_reset = 20
```

---

## Network & mDNS Behavior

- **DHCP**: Upon boot, the script triggers a hardware reset pulse on the W5500 ($\ge 2\,\mu\text{s}$ LOW followed by a $160\text{ ms}$ PLL stabilization delay) and requests an IP address via DHCP. The assigned IP, subnet mask, gateway, and DNS servers are printed to the serial console.
- **mDNS**: CircuitPython's native `mdns.Server` is tightly integrated into the firmware core specifically for `wifi.radio` (Wi-Fi) and cannot bind to SPI-based Ethernet controllers like the W5500. As such, hostname advertisement over mDNS is bypassed on the wired Ethernet interface; clients connect directly to the assigned DHCP IP address printed to the console (or via static DHCP reservations configured on your router).

---

## How to Run

1. Connect the Pico to your PC via USB and plug an Ethernet cable into the USR-ES1 module.
2. Execute the script using `circuit-run` from the project directory:
   ```bash
   ./w5500-lan-test.py
   ```
   `circuit-run` automatically copies the script as `code.py` and transfers tagged dependencies (`config.toml` and [`libs/adafruit_wiznet5k`](file:///home/omakoto/cbin/src/raspberry-pi-pico/circuitpython/libs/adafruit_wiznet5k)) to the `CIRCUITPY` filesystem.

3. Monitor the serial output:
   ```text
   Loaded config from 'config.toml'
   Configuration: hostname='w5500-test', tcp_port=10110
   Executing hardware reset on W5500...
   Initializing WIZNET5K driver with DHCP...
   Ethernet cable connected! Requesting DHCP lease...
   ------------------------------------------------
   Ethernet Connected!
     MAC Address:  00:08:DC:XX:XX:XX
     IP Address:   192.168.1.120
     Subnet Mask:  255.255.255.0
     Gateway:      192.168.1.1
     DNS Server:   192.168.1.1
     Hostname:     w5500-test
   ------------------------------------------------
   TCP Echo Server listening on 192.168.1.120:10110
   Connect via: nc 192.168.1.120 10110
   ```

4. Connect from your computer using `netcat` or `telnet`:
   ```bash
   nc 192.168.1.120 10110
   ```
   Type any message and press Enter; the server logs the incoming message to the serial console and echoes it back.
