# Vendored Third-Party Libraries

Libraries required for the PIO-USB host port (USB-A controller pass-through).
They are vendored here so the firmware builds without network access or extra
submodule setup. Both are MIT licensed; see the LICENSE file in each directory.

## Pico-PIO-USB

- Upstream: https://github.com/sekigon-gonnoc/Pico-PIO-USB
- Version: 0.7.2 (commit `3c1eec341a5232640e4c00628b889b641af34b28`)
- Contents: `src/` only (the PIO-based USB host/device implementation).
- Why this version: 0.7.x adds RP2350 (Pico 2) support. The Pico SDK's bundled
  TinyUSB pins 0.6.1 via its dependency script, which is RP2040-only, so we
  vendor a newer copy and point `PICO_PIO_USB_PATH` at it.

## tusb_xinput

- Upstream: https://github.com/Ryzee119/tusb_xinput
- Commit: `cfd83ba9b0809cf69f7b63d351f44ff73ebd0e30`
  (upstream HEAD targets TinyUSB >= 0.19 where `usbh_class_driver_t::open` returns
  `uint16_t`; this commit matches the 0.18 API bundled with the Pico SDK)
- Contents: `xinput_host.c` / `xinput_host.h` (TinyUSB host class driver for
  XInput controllers: Xbox 360 / Xbox One / Xbox Series wired pads), registered
  through TinyUSB's `usbh_app_driver_get_cb()` hook.

To update either library, replace the files with a newer upstream snapshot and
record the new commit hash here.
