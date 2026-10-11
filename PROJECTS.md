# Major Projects

## nsbackend

Backend for https://github.com/omakoto/raspberry-switch-control. It emulates a USB
Switch controller (HORI Pokken) and takes `nsfrontend` commands over Wi-Fi (TCP, mDNS
`nscon.local:10100`) or serial, so a microcontroller can stand in for the Raspberry Pi.

- [Pico version](pico/nsbackend-pico/) (C++, Pico SDK + FreeRTOS): **Current, the most
  complete.** Runs on Pico / Pico 2 / Pico W / Pico 2 W (Wi-Fi only on the W boards). Adds
  GPIO buttons, an I2C keypad, and a PIO-USB host port that passes a real controller
  (Xbox, Switch Pro, DualShock 4, DualSense, ...) through to the Switch.

- [ESP32 version](esp32/nsbackend-esp32s3/) (C++, ESP-IDF, XIAO ESP32-S3): Port of the Pico
  version. It works, but it has fewer features (I2C keypad only, no controller
  pass-through) and gets less development.

- [CircuitPython version](circuitpython/nsbackend-pico/): The original prototype, for Pico 2 W
  and ESP32. Wi-Fi/TCP and serial only. The C++ versions replace it.

## BT keyboard / mouse multiplexer

Merges several Bluetooth keyboards, mice and trackpads into one composite USB HID device
(keyboard + mouse + VIAL WebHID). It supports per-device keymaps and layers through
[vial.rocks](https://vial.rocks/), and can show status and pairing passkeys on an OLED.

- [ESP32 version](esp32/bt-hid-multiplexer/) (XIAO ESP32-S3 / ESP32-S3-DevKitC-1):
  **Current.** BLE only, because the ESP32-S3 radio has no Bluetooth Classic support.

- [Pico version](pico/bt-hid-multiplexer/) (Pico W / Pico 2 W): The original. It also supports
  Bluetooth Classic devices, but it is stale; newer features are added only to the ESP32
  version.

- [Teensy version](teensy/hid-multiplexer/) (Teensy 4.1): An early prototype of a USB-in,
  USB-out variant. It merges wired USB keyboards and mice plugged into a hub on the Teensy's
  USB host port, with VIAL remapping.

## Stove heat monitor

Watches the stovetop with an MLX90640 32×24 thermal camera and sends a Pushover notification
when a burner has stayed hot (≥ 60 °C by default) longer than a set time (e.g. 15 minutes).

- [CircuitPython version](circuitpython/stove-heat-monitor/) (ESP32-S3): **Current**, the
  only version.
