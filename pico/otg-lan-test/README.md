# otg-lan-test

Turns the native USB port of a **Raspberry Pi Pico / Pico 2 / Pico W / Pico 2 W** into a USB Ethernet adapter, the same trick GP2040-CE uses for its [web configurator](https://gp2040-ce.info/web-configurator/). Plug the board into a PC and it shows up as a network interface; the Pico then hands the PC an IP address over DHCP and serves a web page.

---

## 1. Overview

1. **USB Ethernet gadget**: The board enumerates as a virtual network adapter using TinyUSB's network class. Two USB configurations are offered: **RNDIS** (configuration 1, picked by Windows and Linux) and **CDC-ECM** (configuration 2, picked by macOS).
   The same composite device also carries a **CDC ACM serial console** (`/dev/ttyACM0`) for logs and the **Raspberry Pi reset interface**, so `picotool reboot` and the 1200-baud trick both put the board into BOOTSEL mode.
2. **Static address**: The Pico's side of the link is `192.168.7.1/24` (lwIP, single interface).
3. **DHCP server**: Hands the host PC an address from `192.168.7.2` – `192.168.7.4` with netmask `255.255.255.0`. No default gateway and no DNS server are advertised, so the PC keeps using its normal internet connection for everything outside `192.168.7.0/24`.
4. **HTTP server**: lwIP `httpd` on port 80 serving the contents of [`fs/`](fs/), baked into the firmware at build time. `http://192.168.7.1/` returns `index.html`, which says **Hello world!**
5. **Serial console**: `printf` logs go to the USB CDC console and to UART0. Setting the console to 1200 baud reboots the board into BOOTSEL (handled by `pico_stdio_usb`).
6. **No RTOS**: One bare-metal loop polls TinyUSB, feeds received frames into lwIP, and runs lwIP's timers (`NO_SYS=1`, raw API only).

---

## 2. Hardware Pinout

No external wiring is needed. UART0 is optional, a second copy of the log for a USB-to-serial adapter.

| Function | Pin Name | Default GPIO | Physical Pin # | Details |
| :--- | :--- | :--- | :--- | :--- |
| **USB** | D+ / D- | Native USB | USB Port | Virtual Ethernet adapter (RNDIS / CDC-ECM) + CDC serial console + reset interface |
| **UART0 TX** | GP0 | `GPIO0` | Pin 1 | 115,200 baud, 8N1 / log output (optional) |
| **UART0 RX** | GP1 | `GPIO1` | Pin 2 | 115,200 baud, 8N1 / unused |
| **Status LED** | GP25 | Board LED | Onboard | Pico / Pico 2 only: slow blink waiting for host, fast blink once USB is configured |
| **GND** | GND | `GND` | Pin 3, 8, 13, 18, 23, 28, 38 | Digital ground (for the serial adapter) |

Pico W / Pico 2 W have no GPIO-driven LED (it hangs off the CYW43), so the LED heartbeat is compiled out on those boards.

---

## 3. Build, Install, Test

```bash
./00-build.sh                 # default board: pico (RP2040, non-W)
./00-build.sh -b pico2_w      # or pico_w, pico2
./01-install.sh               # reboots a running board into BOOTSEL and flashes build/otg-lan-test.uf2
./02-monitor.sh               # log console on /dev/ttyACM0 (or a UART adapter on /dev/ttyUSB0)
```

`01-install.sh` first tries `picotool reboot -f -u` (reset interface), then the 1200-baud touch on the CDC console, then waits for the BOOTSEL drive. On a first flash, or if neither works, hold **BOOTSEL** while plugging the board in and run it again.

### Linux host

The kernel binds `rndis_host` and creates an interface such as `usb0` or `enx02xxxxxxxxxx`, and `cdc_acm` creates `/dev/ttyACM0` for the console. NetworkManager runs a DHCP client on the interface automatically:

```bash
ip addr show                   # look for the new interface with 192.168.7.2
curl http://192.168.7.1/       # -> Hello world!
```

Without NetworkManager (or if it is set to ignore the interface), assign an address by hand:

```bash
sudo ip addr add 192.168.7.2/24 dev usb0 && sudo ip link set usb0 up
```

### Windows / macOS

Windows 10+ picks the RNDIS configuration and macOS picks CDC-ECM; both request an address over DHCP. Open `http://192.168.7.1/`.

---

## 4. Layout

| Path | Purpose |
| :--- | :--- |
| `src/main.c` | lwIP setup, TinyUSB net glue, DHCP + HTTP startup, main loop |
| `src/usb_descriptors.c` | Device / RNDIS / CDC-ECM / CDC ACM / reset-interface descriptors, MAC from the flash unique ID |
| `tusb_config.h` | TinyUSB: device mode, `CFG_TUD_ECM_RNDIS=1`, `CFG_TUD_CDC=1` |
| `lwipopts.h` | lwIP: `NO_SYS`, raw API, DHCP-server hook, httpd content file |
| `fs/index.html` | Web root; converted to `pico_fsdata.inc` by the SDK's `makefsdata.py` |

The DHCP server (`dhserver.c`) and the RNDIS control handler (`rndis_reports.c`) are compiled straight from TinyUSB's `lib/networking/` inside the Pico SDK. The serial console, the 1200-baud reset and the picotool reset interface come from the SDK's `pico_stdio_usb`; because this project links TinyUSB itself, those two reset features have to be switched on explicitly in `CMakeLists.txt`.

---

## 5. Changing the page

Edit or add files under `fs/`, list them in `pico_set_lwip_httpd_content(...)` in `CMakeLists.txt`, and rebuild. Files are served by their path relative to `fs/`; `index.html` is what `/` resolves to.
