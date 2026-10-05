# ESP32-S3 BLE HID Multiplexer: Design

Port of [`pico/bt-hid-multiplexer`](../../pico/bt-hid-multiplexer/) (Raspberry Pi Pico W / Pico 2 W,
CYW43439) to the ESP32-S3. Two boards are supported with one pinout:

- **Seeed Studio XIAO ESP32-S3**, the target board ([`ref/ss-esp32-s3.md`](../../ref/ss-esp32-s3.md)).
- **ESP32-S3-DevKitC-1** and compatible boards, the development board
  ([`ref/esp32-s3-devkitc-1.md`](../../ref/esp32-s3-devkitc-1.md)).

The firmware connects up to 8 BLE keyboards, mice and trackpads (HID over GATT) and merges them into one
composite USB HID device (keyboard + mouse, VIAL raw HID, optional CDC console), with VIAL keymapping
over 8 layers, per-device layers, an SSD1306 OLED and a pairing button.

Status: all phases (§13.3) implemented and verified on a DevKitC-1 (ESP32-S3 N8R8): USB, VIAL (vial.rocks), NVS, console, watchdog recovery, flashing over both ports, OLED, button, LED, and pairing, reconnecting and input with a Keychron Nape Pro. Not yet verified: several devices at once (R1) and the XIAO itself.

---

## 1. Decisions

| # | Decision | Why |
|---|---|---|
| D1 | **BLE only. The Bluetooth Classic HID host is dropped.** | The ESP32-S3 radio is BLE 5.0 only; it has no BR/EDR. `classic_hid_host.*` and every classic touchpoint (§9) go away. |
| D2 | **BTstack** (same version as the Pico build, v1.6.2) on the ESP-IDF BLE controller through VHCI, using BTstack's `port/esp32` glue. | `ble_hid_host.cpp` (~2000 lines) and its hard-won device workarounds (§10) carry over almost unchanged. NimBLE/Bluedroid would mean a rewrite and rediscovering every workaround. |
| D3 | **Independent copy** of the Pico sources, adapted in place. No shared code with `pico/bt-hid-multiplexer`. | User decision. Fixes made on the Pico side later have to be ported by hand; §13 lists how to keep the copies easy to diff. |
| D4 | **NVS + console/VIAL** for settings. No `config.toml`, no FATFS, no USB mass storage. | Same model as the Pico: everything is set at runtime and stored in flash. No extra USB drive appears on the host. |
| D5 | ESP-IDF **v5.3** (installed at `~/esp-idf`), target `esp32s3`, managed component `espressif/esp_tinyusb ^2`. | Matches the other `esp32/` projects. |
| D6 | **One owner task for all application state** (the BTstack task). Other tasks only do I/O and talk to it through messages. | BTstack is not thread-safe, and on the Pico the multiplexer, keymap, bindings and BLE host all assumed a single context. Keeping one owner removes every data race listed in §5.3 without adding locks to the ported logic. |
| D7 | The OLED is driven from its own low-priority task with **blocking** I2C. | The Pico's ~25 ms blocking `SSD1306::show()` delayed HID reports. On the S3 it runs on another task, so it can no longer delay input. |
| D8 | **One pinout and one image for both boards.** Every external I/O uses a pin that is on the XIAO header (GPIO1-9, 43, 44), and the boards' own LEDs (which differ: a GPIO LED on the XIAO, a WS2812 on the DevKitC) are not used, so the same firmware runs on both. | The XIAO is the final target; the DevKitC is used for development and exposes the same GPIOs. An earlier version drove each board's LED as a pairing indicator, which needed a per-board build (the DevKitC's LED pins are camera lines on the XIAO Sense); it was dropped to keep a single image. |

---

## 2. Hardware

### 2.1 Boards

| | XIAO ESP32-S3 (target) | ESP32-S3-DevKitC-1 (development) |
|---|---|---|
| Module | ESP32-S3R8, 8 MB flash, 8 MB Octal PSRAM | WROOM-1/2, 4-16 MB flash, optional PSRAM |
| USB | Native USB-C only | Native `USB` port **and** `UART` port (CP2102N/CH343 bridge on GPIO43/44) |
| On-board LED | Not used | Not used |
| Buttons | `BOOT` (GPIO0), `RESET` | `BOOT` (GPIO0), `RESET` |
| Firmware image | the same | the same |

Common to both:
- **Flash.** Build for 4 MB, the smallest common size. It runs on 8 MB and 16 MB parts too.
- **PSRAM.** It is not used. GPIO35-37, which Octal PSRAM takes, are never touched.
- **BOOT button.** It is not used by the firmware. It stays the way to force download mode (hold BOOT, tap RESET) for recovery.
- **USB port labels.** These are swapped on many third-party DevKitC clones. The firmware does not care which port is which.

### 2.2 Pin assignment

All external I/O is on the XIAO header; the DevKitC exposes the same GPIOs. I2C uses the XIAO's default
pins (GPIO5/6), which are also the `esp32/README.md` "universal pinout".

| Function | GPIO | XIAO pin | DevKitC header | Notes |
|---|---|---|---|---|
| OLED SDA | `GPIO5` | `D4` (pin 5) | Row B, silk `5` | I2C0 at 400 kHz (option: 1 MHz). Internal pull-ups on; external 4.7 kΩ recommended. |
| OLED SCL | `GPIO6` | `D5` (pin 6) | Row B, silk `6` | |
| OLED VCC / GND | `3V3` / `GND` | pin 12 / pin 13 | Row B pin 1 / pin 22 | |
| Pairing button | `GPIO4` | `D3` (pin 4) | Row B, silk `4` | Button to GND. Active LOW, internal pull-up. Mounted on the case. |
| USB serial enable | `GPIO7` | `D8` (pin 9) | Row B, silk `7` | Connect to GND before power-up to add the CDC port. Internal pull-up, read once at boot. Next to `GND` on the XIAO's right-hand header. |
| Console UART0 TX / RX | `GPIO43` / `GPIO44` | `D6` (pin 7) / `D7` (pin 8) | wired to the `UART` USB-C bridge | 115200 8N1, always on. On the XIAO it needs an external 3.3 V USB-UART adapter, like the Pico's GP16/GP17. On the DevKitC the on-board bridge provides it. |
| Host USB | `GPIO19` (D-) / `GPIO20` (D+) | USB-C | `USB` USB-C (native OTG) | Composite HID device. |

Unused and free for later: `GPIO1`, `GPIO2`, `GPIO8`, `GPIO9` (XIAO `D0`, `D1`, `D9`, `D10`), and
`GPIO3` (`D2`). GPIO3 is a strapping pin (JTAG source select), so it is the last choice for an input
with a pull-up.

All pins are `#define`s in `main/config.h`, as on the Pico.

**Differences from the Pico wiring.**
- The UART console pins move from GP16/GP17 to `D6`/`D7`. On the DevKitC the on-board bridge is the console, so no adapter is needed.
- There is no pairing LED (the Pico blinks the CYW43 LED).
- I2C moves from GP2/GP3 to `D4`/`D5`, the button from GP6 to `D3`, and the serial jumper from GP10 to `D8`.

---

## 3. Directory layout

```
esp32/bt-hid-multiplexer/
├── DESIGN.md                 this file
├── README.md                 user documentation (rewritten from the Pico README, §12)
├── CMakeLists.txt            ESP-IDF project; adds CFG_TUD_HID_EP_BUFSIZE=32 globally (§6.2)
├── sdkconfig.defaults        all required Kconfig (§11)
├── partitions.csv            nvs 64 KB + phy_init + factory app (§8.1)
├── .gitignore                build/, sdkconfig, sdkconfig.old, managed_components/
├── 00-build.sh               idf.py build (same pattern as esp32/nsbackend-esp32s3)
├── 01-install.sh             flash via UART bridge, or reboot to ROM download over USB (§12.2)
├── 02-monitor.sh -> ../../tools/monitor.sh
├── lastlog.py                previous-run log over VIAL (VID/PID changed, §7.6)
├── gen-vial-layout.py        regenerates main/vial_layout.h (name/VID/PID changed)
├── manual/per-device-mapping.md
├── components/
│   └── btstack/              project-local BTstack component (§4.1)
│       ├── CMakeLists.txt    compiles the BTstack tree from $BTSTACK_ROOT
│       ├── include/btstack_config.h
│       └── port/             copy of BTstack port/esp32 glue (btstack_port_esp32.c, btstack_tlv_esp32.c, headers)
├── main/
│   ├── CMakeLists.txt
│   ├── idf_component.yml     espressif/esp_tinyusb ^2
│   ├── main.cpp              app_main: init, task creation (§5)
│   ├── platform.cpp, platform/platform.h  now_ms(), critical section, reboot helpers (§5.5); the
│   │                         header has its own directory so the host tests' stub can replace it
│   ├── app_task.h/.cpp       BTstack task body, event mailbox, periodic timers (ex main loop)
│   ├── config.h              ported (classic removed, pins changed, flash constants replaced)
│   ├── ble_hid_host.*        ported (§4)
│   ├── multiplexer.*         ported, nearly unchanged
│   ├── virtual_matrix.*      ported, nearly unchanged
│   ├── device_bindings.*     ported, unchanged
│   ├── vial_server.*         ported (UID, bootloader jump)
│   ├── vial_layout.h         regenerated
│   ├── usb_descriptors.*     ported: descriptors handed to esp_tinyusb, HID report descriptors
│   ├── usb_hid.*             TinyUSB glue: install, callbacks, VIAL reply (ex main.cpp callbacks)
│   ├── storage.*             rewritten on NVS (§8)
│   ├── log_ring.*            ported (.noinit RAM, spinlock) (§7.5)
│   ├── dual_console.*        ported: UART0 + CDC I/O task, command table (§7.1)
│   ├── ui_task.*             OLED task (§7.3)
│   ├── ssd1306.*, font_*.h   ported to i2c_master
│   └── button_handler.*      ported
└── test/
    ├── run-host-test.sh      same tests as the Pico (multiplexer, keymap, bindings, log ring)
    ├── host_test.cpp, log_ring_test.cpp
    └── stubs/                platform.h / tusb.h stubs instead of the Pico SDK stubs
```

File names stay the same as on the Pico, except where a file is replaced, so that `diff -r` between the two trees stays useful (§13).

---

## 4. Bluetooth

### 4.1 BTstack integration

BTstack's own `port/esp32/integrate_btstack.py` copies BTstack into `$IDF_PATH/components/btstack`.
That modifies the shared IDF tree, and its stock `include/btstack_config.h` would win over ours. Instead,
the project has its own component:

- **`components/btstack/CMakeLists.txt`** compiles BTstack sources by absolute path from `BTSTACK_ROOT`. The default is `~/pico-sdk/lib/btstack`, which is v1.6.2, the same tree the Pico build uses, so behaviour stays identical. It compiles:
  - `src/*.c` and `src/ble/*.c`
  - `src/ble/gatt-service/hids_client.c` (its `gatt_service_client.c` dependency is in `src/ble`)
  - `3rd-party/micro-ecc/uECC.c`, `3rd-party/rijndael/rijndael.c` (`ENABLE_SOFTWARE_AES128`)
  - `platform/freertos/btstack_run_loop_freertos.c`
  - `platform/embedded/hci_dump_embedded_stdout.c` (optional)
  - the port glue in `port/`

  It does **not** compile `src/classic`, `src/mesh`, lwIP, bluedroid (SBC), LC3, or the audio files.
- **`port/`** holds a copy of `btstack_port_esp32.c`, `btstack_tlv_esp32.c` and their headers from `port/esp32/components/btstack`. They are copied rather than referenced because that directory's `include/` also contains the stock `btstack_config.h`, which must not be on the include path. The copy also lets us adjust:
  - the HCI receive ring size, which drops packets when full (`host_recv_pkt_cb`);
  - logging of those drops into the breadcrumb trail.
- **`include/btstack_config.h`** is ours (§4.2). It is the only `btstack_config.h` on the include path.
- `REQUIRES bt nvs_flash freertos esp_timer`. No lwIP.
- `00-build.sh` fails early with a clear message if `$BTSTACK_ROOT/src/btstack.h` is missing.

### 4.2 `btstack_config.h`

Start from the Pico file. Keep everything BLE-related and replace the CYW43-specific parts with the values from the ESP32 port's stock config.

| Setting | Value | Note |
|---|---|---|
| `ENABLE_LE_CENTRAL`, `ENABLE_LE_PERIPHERAL`, `ENABLE_L2CAP_LE_CREDIT_BASED_FLOW_CONTROL_MODE` | keep | |
| `ENABLE_GATT_LEGACY_CCC_DISCOVERY` | **keep** | ProtoArc XK01 CCCD workaround, comment kept verbatim. |
| `ENABLE_LE_SECURE_CONNECTIONS`, `ENABLE_MICRO_ECC_FOR_LE_SECURE_CONNECTIONS`, `ENABLE_SOFTWARE_AES128` | keep | SC stays compiled in but opt-in (`authreq sc`). |
| `ENABLE_LE_DATA_LENGTH_EXTENSION` | add | The S3 controller supports it. Harmless for HID. |
| `ENABLE_LOG_INFO`, `ENABLE_LOG_ERROR`, `ENABLE_PRINTF_HEXDUMP` | keep | The log filter in `ble_hid_host.cpp` stays. |
| `MAX_NR_GATT_CLIENTS`, `MAX_NR_HCI_CONNECTIONS`, `MAX_NR_HIDS_CLIENTS`, `MAX_NR_SM_LOOKUP_ENTRIES`, `MAX_NR_WHITELIST_ENTRIES` | 8 | |
| `MAX_NR_LE_DEVICE_DB_ENTRIES`, `NVM_NUM_DEVICE_DB_ENTRIES` | 16 | |
| `MAX_NR_L2CAP_CHANNELS` 16, `MAX_NR_L2CAP_SERVICES` 8, `MAX_ATT_DB_SIZE` 512 | keep | |
| `ENABLE_CLASSIC`, `MAX_NR_HID_HOST_CONNECTIONS`, `NVM_NUM_LINK_KEYS`, SCO settings | **remove** | Classic. |
| `MAX_NR_CONTROLLER_ACL_BUFFERS 3`, `HCI_RESET_RESEND_TIMEOUT_MS` | **remove** | CYW43 SPI / reset quirks. The ESP controller reports its buffer counts through `HCI_Read_Buffer_Size`. |
| `HCI_ACL_PAYLOAD_SIZE` | `(1691 + 4)` → **`(255 + 4)`** | BLE needs no BR/EDR-sized payloads. Saves RAM. |
| `HCI_OUTGOING_PRE_BUFFER_SIZE`, `HCI_INCOMING_PRE_BUFFER_SIZE`, `HCI_ACL_CHUNK_SIZE_ALIGNMENT` | as in the port's stock config | `btstack_port_esp32.c` `#error`s without them. |
| `ENABLE_HCI_CONTROLLER_TO_HOST_FLOW_CONTROL`, `HCI_HOST_ACL_PACKET_NUM` 20, `HCI_HOST_ACL_PACKET_LEN` 255+4 | as in the stock config | Also sizes the port's receive ring. |
| `HAVE_FREERTOS_INCLUDE_PREFIX`, `HAVE_FREERTOS_TASK_NOTIFICATIONS`, `HAVE_EMBEDDED_TIME_MS`, `HAVE_ASSERT`, `HAVE_MALLOC` | add | Required by the FreeRTOS run loop and the port. |

### 4.3 Controller (sdkconfig)

- **Mode and stack.**
  - `CONFIG_BT_ENABLED=y` and `CONFIG_BT_CONTROLLER_ONLY=y`, with Bluedroid and NimBLE off.
  - The controller runs in BLE mode (`esp_bt_controller_enable(ESP_BT_MODE_BLE)` in the port).
- **`CONFIG_BT_CTRL_BLE_MAX_ACT=10`.** This is the maximum. Connections, scanning and advertising all count as activities, so 8 connections plus a scan leave 1 spare. The default of 6 would cap the device count.
- **Placement.**
  - `CONFIG_BT_CTRL_PINNED_TO_CORE_0=y`; the controller task priority is 23.
  - The S3 controller keeps its ISRs in IRAM (the `CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY` option exists only for the ESP32-C2), so it keeps running during NVS writes.
- **Power.** `CONFIG_BT_CTRL_MODEM_SLEEP=n` (the default), `CONFIG_PM_ENABLE=n`, and no tickless idle. Low latency matters more than power use here.
- **Scan filtering.** Leave the scan duplicate-filter Kconfig at its default, because the BLE host does its own throttling. Revisit only if advertising reports flood the HCI ring.
- **No Wi-Fi.** It is never initialised, so there is no coexistence time slicing.

### 4.4 Changes to `ble_hid_host.cpp`

The logic stays. Mechanical changes:

1. **Time.** Replace `to_ms_since_boot(get_absolute_time())` with `platform_now_ms()` (§5.5).
2. **Classic removed.**
   - Drop `ClassicHidHost::init/startPairingMode/stopPairingMode/clearBonds` and the include.
   - The classic `init` used to call `gap_set_bondable_mode(1)` globally. Check whether BLE SM bonding relies on it. The BLE code itself only uses `SM_AUTHREQ_BONDING`. If bring-up shows bonds are not stored, call `gap_set_bondable_mode(1)` in `BleHidHost::init`.
3. **Initialisation.** `BleHidHost::init()` keeps its order:
   - `l2cap_init` → `sm_init` → `gatt_client_init` → `att_server_init` → `hids_client_init` → handlers → params → heartbeat → `load_bonded_devices` → `hci_power_control(ON)`
   - It now runs as the `btstack_main()` body on the BTstack task, after the port's `btstack_init()` has set up the VHCI transport, the TLV (NVS) and `le_device_db_tlv`.
   - The Pico got its TLV from `btstack_cyw43_init`; here `btstack_init()` provides it.
4. **Public API.** Every public `BleHidHost::` function is called **only on the BTstack task** (D6). The console, button and VIAL paths reach it through the mailbox (§5.3).
   - The getters that the UI reads (`getConnectedCount`, `getConnectedDeviceName`, `getActivePasskey`, `isPairingMode`) are not called cross-task. They feed the UI snapshot instead (§5.4).
5. **Heartbeat.** `isAlive()` is no longer needed. The 1 s heartbeat timer feeds the task watchdog directly (§7.5).
6. **Pairing-mode scan check.** The Pico's main-loop "every 1000 ms: start scanning if there are unconnected bonds or pairing mode" becomes a 1 s BTstack timer.
7. **Host LED sync.** Also moves off the main loop: `Multiplexer::setHostLeds` now runs on the BTstack task, so the LED sync is done right there.

Everything in §10 must survive the port unchanged.

---

## 5. Tasks and concurrency

### 5.1 Task layout

| Task | Core | Prio | Stack | Owner of | Notes |
|---|---|---|---|---|---|
| BT controller (IDF) | 0 | 23 | IDF | radio | Fixed by Kconfig. |
| esp_timer (IDF) | 0 | 22 | IDF | | |
| **`bt_app`** (BTstack run loop) | 0 | 19 | 8 KB | **all application state**: BTstack, `BleHidHost`, `Multiplexer`, `VirtualMatrix`, `DeviceBindings`, `VialServer`, `StorageManager` writes, button logic | Runs `btstack_init(); btstack_main(); btstack_run_loop_execute();` and never returns. It is on the same core as the controller, so VHCI hand-offs stay core-local. |
| TinyUSB (`esp_tinyusb`) | 1 | 20 | 4 KB | `tud_task()` | Set through `tinyusb_config_t.task`. Its priority is above the UI and console tasks so that endpoint completions are handled at once. |
| `console` | 1 | 3 | 4 KB | UART0 driver, CDC FIFO, log ring drain, line editing | Wakes every 5 ms or on UART RX. |
| `ui` | 1 | 2 | 4 KB | I2C bus, SSD1306 frame buffer | Renders from the snapshot. A blocking 1 KB I2C transfer here stalls nothing else. |
| `app_main` | 0 | 1 | | | Initialises, creates the tasks and returns. |

### 5.2 The rule

Only `bt_app` reads or writes application state and calls BTstack.
- Other tasks send it **messages**.
- `bt_app` publishes a read-only **snapshot** for the UI.
- The log ring is the only structure written by several tasks, and it has its own spinlock (§7.5).

This reproduces the single-context assumption the Pico code was written under. On the Pico, the BTstack IRQ context preempted the main loop without locks. On the S3 that would be a real multi-core race.

### 5.3 Messages into `bt_app`

`btstack_run_loop_execute_on_main_thread()` is safe from any task. It takes a `btstack_context_callback_registration_t`, which must outlive the call, so each message kind has its own static registration plus a small payload slot:

| Event | From | Payload / handling on `bt_app` |
|---|---|---|
| Keyboard LED output report | TinyUSB `tud_hid_set_report_cb` (inst 0) | Store the latest byte in an atomic and post `on_host_leds`. That runs `Multiplexer::setHostLeds` followed by `BleHidHost::sendHostLeds`. |
| VIAL request | TinyUSB `tud_hid_set_report_cb` (inst 1) | Copy the 32 B request into a 4-entry FreeRTOS queue and post `on_vial`. That runs `VialServer::handleRawReport` and then sends or retries the reply (§6.4). |
| HID IN endpoint free | TinyUSB `tud_hid_report_complete_cb` (inst 0/1) | Post `on_usb_ready`. That runs `Multiplexer::flushKeyboard/flushMouse` and the VIAL reply retry. |
| USB mounted, unmounted, suspended | TinyUSB callbacks | Set a flag. The UI shows USB health. |
| Console line | `console` task | Copy the line (≤128 B) into a 4-entry queue and post `on_console_line`. `handle_command()` runs there, as on the Pico. Its output goes through `printf` into the log ring. |
| CDC 1200-baud touch | TinyUSB `tud_cdc_line_coding_cb` | Post `on_bootloader_request`. |

**Ordering is kept.** Registrations go into a list, and queues are FIFO.

**Overflow is visible.**
- A VIAL request that does not fit is dropped with a log line. vial.rocks retries.
- A console line that does not fit prints `busy`.

The message list must also hold periodic work that was previously polled from the main loop. All of these become BTstack timers on `bt_app`:

| Pico main loop stage | ESP32 timer on `bt_app` |
|---|---|
| `VirtualMatrix::flushPendingSave()` every pass | 100 ms timer (the 500 ms debounce is unchanged) |
| `Multiplexer::flush*()` every pass | Event driven: on report arrival (as before), on `on_usb_ready`, and when USB is mounted. Output is only ever left pending after a send attempt found the endpoint busy, and the transfer occupying it always ends with a completion or failure callback (both post `on_usb_ready`), so no polling timer is needed. |
| `ButtonHandler::update()` | 10 ms timer, reading `GPIO4` with `gpio_get_level`. |
| Scan check (1 s) | 1 s timer |
| OLED state-change detection | `bt_app` publishes the snapshot on every change. The `ui` task compares and redraws. |
| Watchdog feed | 1 s heartbeat timer (§7.5) |

### 5.4 UI snapshot

```c++
struct UiSnapshot {            // written by bt_app only
    uint8_t  connected_count;
    char     device_name[32];
    int8_t   active_layer;     // VirtualMatrix::getEffectiveLayer(DeviceBindings::lastActiveDevice())
    bool     pairing;
    uint32_t passkey;
    char     toast[32];
    uint32_t toast_expiry_ms;
    bool     usb_mounted;
    uint32_t seq;              // incremented on every publish
};
```

- **Publishing.** `bt_app` fills a local copy and publishes it with a `portMUX` critical section (a 100 B memcpy). It then `xTaskNotify`s the `ui` task.
- **Reading.** The `ui` task copies the snapshot under the same lock and renders outside it.
- **When `bt_app` publishes.** After any event that can change a field: connection or disconnection, report activity that changes `lastActiveDevice` or the layer, a pairing change, a passkey, a toast, or USB state.
  - Computing the snapshot is cheap. It is a handful of getters, as in the Pico main loop's change detection.
  - It is published at most every 20 ms. A burst of key reports does not spin the UI, but the layer display still reacts within one frame.

### 5.5 `platform.h`

A small header used by the ported sources in place of the Pico SDK.
- `uint32_t platform_now_ms()`: `esp_timer_get_time() / 1000`.
- `platform_critical_enter/exit()`: a `portMUX_TYPE` spinlock (`portENTER_CRITICAL`) for the log ring.
- `platform_reboot()`: `esp_restart()`.
- `platform_reboot_to_download_mode()`: see §6.5.

The host-test stubs implement the same header (§13.2).

---

## 6. USB

### 6.1 Stack

- **Component.** Use `espressif/esp_tinyusb ^2` with our own descriptors, passed via `tinyusb_config_t.descriptor` (`device`, `string[]`, `full_speed_config`). This is the approach `nsbackend-esp32s3` already takes.
- **Kconfig.** `CONFIG_TINYUSB_HID_COUNT=2`, `CONFIG_TINYUSB_CDC_ENABLED=y`, `CONFIG_TINYUSB_CDC_COUNT=1`, `CONFIG_TINYUSB_MSC_ENABLED=n`.
- **Task.** esp_tinyusb creates the `tud_task` task (§5.1).
- **Console port.** USB-Serial-JTAG shares the PHY and is gone once TinyUSB installs it. The console is therefore on UART0, and `CONFIG_ESP_CONSOLE_UART_DEFAULT` stays set so that boot and panic messages reach GPIO43 (the DevKitC's bridge, or an adapter on the XIAO's `D6`).

### 6.2 HID OUT buffer size: must be 32

On the Pico, `tusb_config.h` sets `CFG_TUD_HID_EP_BUFSIZE 32`, for this reason: "TinyUSB arms the OUT endpoint for this many bytes and a transfer only completes at a short packet or when the buffer is full, so a larger value leaves every 32-byte VIAL request pending until the next one arrives."

- esp_tinyusb's private `tusb_config.h` does not set this value, so TinyUSB defaults it to 64. That would reintroduce the hang.
- TinyUSB's `hid_device.h` wraps the default in `#ifndef`. So the project `CMakeLists.txt` adds the definition to every component before `project()`:

  ```cmake
  idf_build_set_property(COMPILE_DEFINITIONS "CFG_TUD_HID_EP_BUFSIZE=32" APPEND)
  ```

- Reports still fit: the keyboard report is 8 B plus the ID, the mouse report 5 B plus the ID, and VIAL 32 B.
- A comment at that line carries the reason above.

### 6.3 Descriptors

Same structure as the Pico (`usb_descriptors.c`), with new identity strings.

**Device.**
- `bcdUSB 0x0200`.
- Class: MISC/IAD with CDC, 0/0/0 without CDC.
- VID `0x303A` (Espressif).
- PID: `USB_PID` in `config.h`, `0x4004` (esp_tinyusb's generic "HID" PID; decided, Q1). It is shared with other TinyUSB HID gadgets, so the scripts identify the device by VID/PID **and** the product string or the `06 60 FF` report descriptor prefix, never by VID/PID alone.
- `bcdDevice 0x0100`.

**Strings.** esp_tinyusb limits strings to 31 characters and 8 descriptors.

| Index | String |
|---|---|
| 0 | `0x0409` |
| 1 | `omakoto` |
| 2 | `ESP32-S3 BLE HID Multiplexer` (28 characters) |
| 3 | Serial: the base MAC from `esp_efuse_mac_get_default` as 12 hex characters |
| 4 | `ESP32-S3 Serial Console` |
| 5 | `ESP32-S3 Keyboard/Mouse` |
| 6 | `ESP32-S3 VIAL Configurator` |

**Interfaces with CDC.** Endpoint numbers must be below 7 on the S3's DWC2, and IN endpoints are limited to 5 including EP0. With CDC, all 4 non-EP0 IN endpoints are used:

| Interface | Function | Endpoints |
|---|---|---|
| 0, 1 | CDC ACM | notification `0x81`, OUT `0x02`, IN `0x82` |
| 2 | HID keyboard (report ID 1, boot-compatible 8 B) + mouse (report ID 2: buttons, x, y, wheel, pan), polled every 1 ms | OUT `0x03`, IN `0x83` |
| 3 | HID VIAL raw (`0xFF60`/`0x61`, 32 B in and 32 B out, no report ID), polled every 1 ms | OUT `0x04`, IN `0x84` |

**Without CDC.** Interfaces 0 and 1 are HID, with the same endpoints.

**CDC on or off.** The choice is made once at boot, before `tinyusb_driver_install`:
- CDC is on if `GPIO7` is low after a 200 µs pull-up settle, or if the build defines `USB_SERIAL_ALWAYS` (`./00-build.sh -D USB_SERIAL_ALWAYS=ON`).
- The matching device and configuration descriptor pair is then passed to esp_tinyusb.

### 6.4 Sending reports from `bt_app`

`Multiplexer` calls `tud_hid_n_ready()` and `tud_hid_n_report()` / `tud_hid_n_mouse_report()` directly from `bt_app`, as the Pico did from its BTstack context.

**Why this is safe.**
- With `CFG_TUSB_OS = OPT_OS_FREERTOS`, TinyUSB's `usbd_edpt_claim()` is guarded by `_usbd_mutex` (`usbd.c:424-576, 1559`). Only one task can start a transfer on an endpoint.
- Only `bt_app` sends on HID instances 0 and 1, so there is no contention on those endpoints.
- `tud_task` on core 1 completes the transfer and fires `tud_hid_report_complete_cb`, which posts `on_usb_ready` (§5.3). This replaces the Pico's "flush again on the next main-loop pass".

**Fallback.** Bring-up must confirm that `dcd_edpt_xfer` from a task other than `tud_task` is reliable on the DWC2 port. If it is not, the send is done with `usbd_defer_func(send_fn, nullptr, false)`, which runs inside `tud_task`, and the report goes through a one-slot mailbox per instance.

**VIAL replies.**
- The reply is built on `bt_app` into `s_vial_reply`.
- `bt_app` sends it at once if instance 1 is ready.
- Otherwise it retries on `on_usb_ready`. This keeps the Pico rule: "a dropped reply leaves the web configurator waiting forever".

### 6.5 Bootloader jump (VIA `0x0B`, console `bootloader`, CDC 1200 baud)

`platform_reboot_to_download_mode()`, which replaces `reboot_to_bootsel()`:

1. Print a message, then `LogRing::flush(300)`.
2. Wait until the VIAL reply has gone out. This keeps the Pico ordering: reply first, then reboot.
3. `tud_disconnect()`, then wait 150 ms.
4. Take `bt_app` off the task watchdog with `esp_task_wdt_delete`, so the watchdog cannot fire during the reset sequence.
5. Hand the internal USB PHY back to the USB-Serial-JTAG controller (clear `RTC_CNTL_SW_HW_USB_PHY_SEL` and `RTC_CNTL_SW_USB_PHY_SEL` in `RTC_CNTL_USB_CONF_REG`). TinyUSB routed it to the OTG controller, and that routing is in the RTC domain, which survives the restart. Without this step the ROM's download port never appears on the native USB port (verified).
6. `REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT); esp_restart();`
   - This is what IDF itself does in `esp_system/port/usb_console.c`.
   - The ROM then enters **download mode**. It is reachable on UART0 (the DevKitC bridge, or an adapter on the XIAO) and, on the S3, as the ROM USB-Serial-JTAG device `303a:1001` on the native port.
   - Verified on the DevKitC: `01-install.sh` reboots the board this way and flashes it over the native port. It would not work on a chip with the `DIS_USB_SERIAL_JTAG` or `DIS_FORCE_DOWNLOAD` eFuse burned.

---

## 7. Peripherals and services

### 7.1 Console (`dual_console.cpp`)

- **UART0.** `uart_driver_install(UART_NUM_0, 1024 RX, 4096 TX)`.
  - Output: the `console` task checks `uart_get_tx_buffer_free_size()` before every `uart_write_bytes`, so it **never blocks**. This keeps the Pico's "console output never blocks" property.
  - Input: `uart_read_bytes(..., 0)`.
- **CDC**, when enabled.
  - `tud_cdc_n_write_available` / `tud_cdc_n_write` / `tud_cdc_n_write_flush`.
  - All CDC calls come from the `console` task. TinyUSB's CDC FIFO is mutex-protected under FreeRTOS, and there is a single writer anyway.
  - DTR rising edge: the welcome banner, as on the Pico.
  - 1200 baud: bootloader jump.
- **Redirecting `stdout` and ESP_LOG into the log ring.** The ported code uses `printf` everywhere, and BTstack logs through `printf` too.
  - `stdout`: in `app_main`, before any task is created, set `_GLOBAL_REENT->_stdout` (and the main task's `stdout`) to a `funopen()` stream whose write function is `LogRing::write`. Make it unbuffered with `setvbuf(..., _IONBF, 0)`. Tasks created later inherit it.
  - ESP_LOG: `esp_log_set_vprintf()` formats into a 256 B stack buffer, then calls `LogRing::write`. It must check `xPortInIsrContext()` and must not use FreeRTOS calls, because it can be called with the cache disabled.
  - Panic and boot ROM output still go straight to UART0, which is what we want after a crash.
- **Commands.** The command table is copied from the Pico. It is parsed on the `console` task and executed on `bt_app` (§5.3).
  - Classic commands and help lines are removed (§9).
  - `bootloader` / `bootsel` reboots into download mode.
  - `hangtest` hangs `bt_app` on purpose, which is now the right target, because `bt_app` owns everything.
  - Keep the prefix rule for `log` and `leds`, which need a trailing space or end of line.
  - Add `reboot` (plain `esp_restart`).
  - Line input is unchanged: 128 B per source, CR/LF, backspace/DEL.

### 7.2 Button (`button_handler.cpp`)

- The logic is unchanged: 30 ms debounce; a short press is 50-1500 ms; a long press fires at 2 s while held; factory reset fires at 8 s while held.
- Input is one button on `GPIO4` (XIAO `D3`) to GND, with the internal pull-up. The on-board BOOT button is not used, so pressing it never interferes with pairing (§2.1).
- The 8 s factory reset does the same as on the Pico: it clears bonds and resets the keymap, but keeps the per-device layer bindings. Only the `reset` console command clears the bindings too. This is a deliberate decision to stay at parity (Q4).

### 7.3 OLED (`ssd1306.cpp`, `ui_task.cpp`)

- **Bus.** The new `driver/i2c_master.h`: `i2c_new_master_bus` on I2C0 (SDA 5, SCL 6, internal pull-ups, glitch filter 7), then `i2c_master_bus_add_device` at 0x3C and 400 kHz. 1 MHz is selectable in `config.h`; most SSD1306 modules handle it.
- **Transfers.** Blocking `i2c_master_transmit` from the `ui` task. Async I2C is marked experimental in IDF 5.3 and is not needed, because nothing else waits on this task.
- **Code.** The drawing code, fonts and screen layouts are unchanged: boot splash, status, passkey, pairing, toast.
  - The splash shows `Firmware: v1.0.0`.
  - The status screen adds a small USB state marker when the device is not mounted. The README already promises "USB connection health".
- **Redraw policy.** Redraw on snapshot change or every 10 s, as on the Pico. Toast expiry is checked by the `ui` task itself.
- **Missing display.** If the OLED does not ACK at init, log it once and keep running without a display. This is the same as the Pico, which tolerated a missing display.

### 7.4 Pairing LED (dropped)

There is none: the boards' own LEDs differ (a GPIO LED on the XIAO, a WS2812 on GPIO48/GPIO38 on the
DevKitC, whose pins are camera lines on the XIAO Sense), and driving them would need a per-board build.
The OLED shows pairing mode instead.

### 7.5 Log ring, breadcrumbs, watchdog, previous-run log

**Log ring** (`log_ring.cpp`): the code is ported nearly verbatim (8 KB ring, 16 breadcrumbs, magic `'LOGR'`, the same drain and resync logic).

- **Placement.** `static PersistentLog s_log __NOINIT_ATTR;` in internal DRAM `.noinit`.
  - IDF documents `.noinit` as kept across software restarts. That covers `esp_restart`, panic and watchdog resets.
  - Power-on and brown-out leave garbage. The existing magic and sanity check handle that.
  - RTC no-init memory is too small for 8 KB plus the IDF's own use.
  - If bring-up shows that the second-stage bootloader overwrites the region, fall back to `RTC_NOINIT_ATTR` with a 4 KB ring.
- **Locking.** `save_and_disable_interrupts()` becomes `platform_critical_enter/exit` (a spinlock), because `bt_app`, `console`, `ui`, TinyUSB and ESP_LOG can all write from both cores. The critical section covers only the memcpy and the index update, as before.
- **"Was it a watchdog reset".** `esp_reset_reason()` ∈ {`ESP_RST_TASK_WDT`, `ESP_RST_INT_WDT`, `ESP_RST_WDT`, `ESP_RST_PANIC`}. The report labels which one, so a panic is reported as a panic rather than a watchdog.

**Watchdog.** This replaces `watchdog_enable(5000)` plus the BT heartbeat.

- **Configuration.** `CONFIG_ESP_TASK_WDT_EN=y`, `CONFIG_ESP_TASK_WDT_TIMEOUT_S=5`, `CONFIG_ESP_TASK_WDT_PANIC=y`, and `CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT` (the default).
- **Feeding.** `bt_app` subscribes with `esp_task_wdt_add(NULL)` and feeds from its 1 s heartbeat BTstack timer. If the run loop wedges, which also covers a BTstack hang, the timer stops and the board reboots after 5 s. This is the same semantics as the Pico's `isAlive()` gating.
- **Other tasks.** `console` and `ui` subscribe as well and feed every loop iteration.
  - The `ui` task's worst case is one I2C transfer of about 25 ms. It uses an I2C timeout of 100 ms, so a stuck bus does not trip the watchdog.
- **Idle-task checks.** `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0/1` stay on.

**Breadcrumbs** (`LogRing::stage()`).
- **Codes.** `0x1xxx` HCI, `0x2xxx` SM, `0x3xxx` GATT, `0x5001`-`0x5004` storage, and `0x6001`/`0x6002` pairing are kept. `0x4xxx` (classic) is retired, and the number is not reused.
- **Stages.** On the Pico, stages were positions in the main loop. On the S3 they mark which handler `bt_app` is running:

  | Stage | `bt_app` is running |
  |---|---|
  | 1 | USB event |
  | 2 | console command |
  | 3 | report flush |
  | 4 | button |
  | 5 | scan or periodic timer |
  | 6 | UI snapshot publish |
  | 7 | VIAL request (new) |
  | 8 | storage write (new) |
  | 99 | `hangtest` |

  Each handler sets its stage on entry and resets it to 0 on exit. Stage 0 at the time of the reset means `bt_app` was idle and some other task hung.
- **Panic output.** The panic handler's own "task watchdog got triggered, tasks: X" goes to UART0 only. Optionally, `esp_task_wdt_isr_user_handler()` (available when `CONFIG_ESP_TASK_WDT_PANIC=y`) can add a breadcrumb `0x7001` before the panic. This is a nice-to-have.

**Previous run.**
- After a watchdog or panic reboot, the summary prints after the banner, as on the Pico.
- The full log is available from `lastlog` on the console, or from `./lastlog.py` over VIAL.
- The private `0xFD 0x00` / `0xFD 0x01` commands are unchanged.

### 7.6 VIAL

- **Server.** `vial_server.cpp` is unchanged, apart from these items:
  - **UID.** The VIAL keyboard UID changes from `PICO2WMX` to `ESP32SMX`, so vial.rocks treats the two as different keyboards and does not mix up their saved layouts.
  - **Protocol version and keycodes.** The VIAL protocol version stays 3. The keycode numbering in `config.h` depends on it.
  - **`0x0B` bootloader jump.** It sets the flag; `bt_app` sends the reply, then calls `platform_reboot_to_download_mode()` once the reply is out.
- **Layout.** `gen-vial-layout.py` gets the new name, VID and PID, and regenerates `vial_layout.h`.
- **`lastlog.py` / `01-install.sh`.** They find the hidraw node by `0003:0000303A:<PID>` plus the `06 60 FF` report-descriptor prefix.
- **Linux permissions.** `~/cbin/setup/config-hidraw-permission` currently covers VID `2e8a` but not `303a`, so vial.rocks and `lastlog.py` would hang on "Connecting..." without root.
  - VID `303a` has been added to that script and its test (F1, done). The rule takes effect after the script is run again.

---

## 8. Persistence (`storage.cpp` on NVS)

### 8.1 Partition table (`partitions.csv`)

```csv
# Name,   Type, SubType, Offset,  Size
nvs,      data, nvs,     0x9000,  0x10000
phy_init, data, phy,     0x19000, 0x1000
factory,  app,  factory, 0x20000, 0x200000
```

- **nvs (64 KB, up from the default 24 KB).** BTstack's TLV (namespace `BTstack`) and the app data (namespace `bthidmux`) share it. NVS needs free pages for garbage collection, and a rewrite of a keymap layer must always find room.
- **factory (2 MB).** Leaves headroom for BTstack, TinyUSB and the drivers, all of which fit in about 1 MB. The table fits a 4 MB flash.

### 8.2 Data

| NVS key (namespace `bthidmux`) | Contents | Replaces |
|---|---|---|
| `km_hdr` | `{magic 'VIAL', version 5, layers 8}` | Keymap header |
| `km0` … `km7` | One 512 B blob per layer (16×16 `uint16_t`) | The 4 KB keymap in 2 flash sectors |
| `bind` | `DeviceBindingStorageData` (magic `'BIND'`, version 1, 8 entries) | Bindings sector |

- **Checksums.** NVS stores a CRC per entry, so the Pico's own checksums become redundant. The magic and version stay, to detect layout changes.
- **No migration.** There are no legacy formats to load (no 4-layer keymap, no legacy bindings offset), because this firmware starts fresh.
- **Writes.**
  - Keymap: debounced by 500 ms, as before. Only the layers whose contents changed are written, compared against a shadow copy of the last saved data. A typical VIAL edit therefore rewrites 512 B, not 4 KB.
  - `resetKeymap` writes all 8 layers.
  - Bindings: written at once on `bind`, `unbind` and `clearAll`.
  - All writes happen on `bt_app`, as on the Pico, where writes ran in the main loop with interrupts off.
- **Cost of a write.** A flash erase or write stalls both cores' cache. BT controller ISRs are in IRAM and keep running. The DWC2 USB interrupt is not IRAM-safe and is delayed by the length of the write. This is the same trade-off the Pico made ("tens of ms" with interrupts off), and debouncing keeps writes rare.
- **BTstack bonds.** BTstack's `btstack_tlv_esp32` stores bonds in NVS namespace `BTstack`, committing on every store. `ble_hid_host.cpp`'s own `HOGT` bonded-table tag goes through the same TLV, unchanged. The legacy `HOGD` migration code is kept unchanged (harmless, and it keeps the file close to the Pico's).
- **Not persisted, as on the Pico:** mouse speed, `authreq`, log toggles (decided, Q3).
- **Factory reset**, as on the Pico:
  - `reset` (console) clears the BTstack bonds through the existing `clearBonds` (TLV deletes), resets the keymap, and clears the bindings.
  - The 8 s button press clears the bonds and resets the keymap, and keeps the bindings (Q4).
- **Last resort.** `idf.py erase-flash` resets everything.

---

## 9. Removed: Bluetooth Classic

Removed:

- **Source files.** `classic_hid_host.{h,cpp}`.
- **`config.h`.** `MAX_CLASSIC_DEVICES`. `MAX_KEYBOARDS` and `MAX_MICE` become `MAX_BLE_DEVICES` (8). That shrinks `active_translation_` from 12×256 to 8×256 entries, as well as the multiplexer arrays and the bindings cache.
- **`main.cpp`.** The classic fallbacks in `device_address()`, LED sync, connected count, device name and passkey.
- **`ble_hid_host.cpp`.** The `ClassicHidHost::init`, `startPairingMode`, `stopPairingMode` and `clearBonds` calls, and the include.
- **Console.** `cconnect`, `cdisconnect`, `cqos`, the classic parts of `status`, `devices` and `bonds`, the `(classic)` name placeholder, the help lines, and the `0x4xxx` breadcrumb legend.
- **Breadcrumbs.** `CRUMB_CLASSIC`. `lastlog.py` keeps decoding `0x4` as "classic (unused)".
- **README.** The "Bluetooth Classic devices" section and every classic mention. A short note in their place says why: the S3 is BLE only.

---

## 10. Behaviour that must be preserved (from the Pico code)

Copy every comment and keep the code.

1. **CCCD discovery.** Use Read-By-Type CCCD discovery (`ENABLE_GATT_LEGACY_CCC_DISCOVERY`). Without it, MTU-23 peripherals such as the ProtoArc XK01 lose their CCCD writes.
2. **Pairing defaults.**
   - Pair with LE legacy pairing without MITM by default. Secure Connections is opt-in through `authreq sc`, because the XK01 never sends notifications after SC pairing.
   - IO capability is `DISPLAY_ONLY`.
3. **Pairing delay.** Wait 200 ms after connecting before calling `sm_request_pairing`.
4. **No re-enable after connect.** Do not call `hids_client_enable_notifications` after `HID_SERVICE_CONNECTED`.
5. **Early reports.** Accept HID reports before `HID_SERVICE_CONNECTED`, and mark the slot connected on the first report.
6. **Mouse report ID.** The mouse report ID from the descriptor wins over the "ID 1 = keyboard" guess (Keychron M5 8K). Without a descriptor ID, assume ID 2.
7. **Zero slave latency** (`BLE_ZERO_SLAVE_LATENCY`).
   - Send the request 10 s after the last parameter update, at most 3 times, at the shortest interval observed.
   - Recompute the supervision timeout.
8. **Pairing policy.**
   - Pairing is non-aggressive: unbonded devices are only considered during pairing mode.
   - "Bonded" means present in the table or in `le_device_db`.
9. **Connect handling.**
   - Connect/cancel: a 10 s timeout calls `gap_connect_cancel`, and the cleanup happens on the `CONNECTION_COMPLETE` error.
   - Stop scanning before `gap_connect`.
   - `startScan` while connecting cancels the connection.
10. **LRU eviction.** Queue the pending connect, `gap_disconnect(lru)`, and connect on `DISCONNECTION_COMPLETE`.
11. **Stale bond.** If re-encryption fails, remove the stale `le_device_db` entry, then `sm_request_pairing`.
12. **Slot reset.** `reset_slot` removes the latency timer before zeroing the slot.
13. **Host LEDs.** Parse the LED output report ID from the descriptor (usage page `0x08`), and write the current LEDs on connect.
14. **BTstack log filter.**
    - Let through only `sm.c`, `gatt_client.c` and `hids_client.c`, plus all errors.
    - Verbose dumps switch off after 15 s.
    - Reports are capped at 40 lines/s.
    - On the S3, console output is non-blocking, so the filter now protects the log ring's 8 KB rather than USB enumeration. It stays.
15. **Init order.** Keep the init order of §4.4.
16. **Key release.** `processKeyRelease` uses the stored `active_translation_` and the `HELD_KC_NO` sentinel.
17. **Taps.** The tap queue sends a release between repeated taps.
18. **Purge on disconnect.** `purgeKeyboard` / `purgeMouse` run on every disconnect, including `clearBonds`.
19. **Mouse clamp.** Mouse axes clamp to ±127, and the remainder carries to the next report.
20. **Keymap saves.** Debounced by 500 ms.
21. **VIAL protocol.**
    - Version 3, with the matching keycode numbering.
    - QMK-settings query replies `0xFF…` (anything else polls forever).
    - Always unlocked.
22. **VIAL replies.** Retried until the IN endpoint is free.
23. **HID OUT buffer.** `CFG_TUD_HID_EP_BUFSIZE == 32` (§6.2).
24. **CDC presence.** Decided before USB starts, after a 200 µs settle.
25. **Bootloader jump.** Reply first, then disconnect USB, then reboot.

The host tests (§13.2) cover items 16-20 and the log ring.

---

## 11. `sdkconfig.defaults`

```ini
CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_FREERTOS_HZ=1000

# Bluetooth: controller only (BTstack is the host), BLE, max activities
CONFIG_BT_ENABLED=y
CONFIG_BT_CONTROLLER_ONLY=y
CONFIG_BT_CTRL_BLE_MAX_ACT=10
CONFIG_BT_CTRL_PINNED_TO_CORE_0=y
CONFIG_BT_CTRL_MODEM_SLEEP=n

# No power management (latency over power)
CONFIG_PM_ENABLE=n

# TinyUSB: 2 HID + 1 CDC (CDC is only in the descriptor when enabled at boot)
CONFIG_TINYUSB_HID_COUNT=2
CONFIG_TINYUSB_CDC_ENABLED=y
CONFIG_TINYUSB_CDC_COUNT=1
CONFIG_TINYUSB_MSC_ENABLED=n

# Console on UART0 (GPIO43/44: DevKitC bridge, or an adapter on XIAO D6/D7); boot/panic output stays there
CONFIG_ESP_CONSOLE_UART_DEFAULT=y
CONFIG_ESP_CONSOLE_UART_BAUDRATE=115200

# Watchdog: 5 s, panic -> reboot, previous run's log kept in .noinit
CONFIG_ESP_TASK_WDT_EN=y
CONFIG_ESP_TASK_WDT_TIMEOUT_S=5
CONFIG_ESP_TASK_WDT_PANIC=y

CONFIG_ESP_MAIN_TASK_STACK_SIZE=4096
CONFIG_COMPILER_OPTIMIZATION_PERF=y
```

Exact Kconfig names are checked against `~/esp-idf` (v5.3) when the file is written. Names marked `[B]` in the research notes, such as the panic-reboot option, are verified then.

---

## 12. Build, flash, docs

### 12.1 `00-build.sh`

- Same pattern as `esp32/nsbackend-esp32s3/00-build.sh`. It sources `${IDF_PATH:-~/esp-idf}/export.sh` and runs `idf.py build`.
- **Additions:**
  - Supports `-h`/`--help` (getopt).
  - Checks `BTSTACK_ROOT`, defaulting to `~/pico-sdk/lib/btstack`.
  - `-D NAME=VALUE` passes CMake definitions, e.g. `-D USB_SERIAL_ALWAYS=ON|OFF`.

- **First build only:** runs `idf.py set-target esp32s3` if `sdkconfig` is missing.

### 12.2 `01-install.sh`

Supports `-h`/`--help`. The port can be given with `-p` or through `ESPPORT`. Otherwise, the same on
both boards:

1. **Native USB port.** Use the ROM USB-Serial-JTAG download port (`303a:1001`,
   `/dev/serial/by-id/*Espressif*USB_JTAG*`) if it is already there; otherwise ask the running firmware
   to enter download mode, then wait up to 6 s for it:
   1. **VIA `0x0B` over hidraw.** Find the node by `303A:<PID>` plus the `06 60 FF` descriptor prefix. Works without CDC.
   2. **CDC console.** If the CDC console is present: `bootloader\r\n`.
2. **UART bridge (fallback).** If that does not work and a CP210x or CH34x port is present (the
   DevKitC's `UART` port), flash through it: the esptool DTR/RTS auto-reset needs no firmware
   cooperation. A plain adapter on the XIAO's `D6`/`D7` cannot reset the chip.
3. **Flash** with `idf.py -p <port> flash`. esptool's default `--after hard_reset` restarts into the new firmware.
4. **Recovery.** If nothing responds, print instructions: hold BOOT, tap RESET, release BOOT, then rerun. This is how the very first flash of a XIAO (or a board running other firmware) is done.

The Pico's UF2 and `picotool` logic is not carried over.

### 12.3 README

Rewrite the Pico README for the S3:
- wiring table (§2.2)
- build and flash (§12)
- console commands without classic
- the VIAL steps, with the `303a` udev note
- hang recovery: `.noinit` plus the task watchdog
- latency notes
- the "Pairing policy", "CCCD discovery" and "Input latency" sections, carried over as they are
- the per-device mapping manual, copied

Update `esp32/README.md` §5 (the project list) to add the new project.

---

## 13. Keeping the copy maintainable; testing

### 13.1 Diffability (D3)

- Keep file names, class names and function order identical to the Pico sources.
- Pico SDK calls are replaced through `platform.h`, not rewritten inline. A `diff -r pico/bt-hid-multiplexer/src esp32/bt-hid-multiplexer/main` then shows mostly real logic differences.
- The first commit of the port is a straight copy of the Pico `src/`, with the build changes in later commits. `git log -p` then shows exactly what the port changed.

### 13.2 Host tests

- `test/run-host-test.sh` builds `multiplexer.cpp`, `virtual_matrix.cpp`, `device_bindings.cpp` and `log_ring.cpp` with `g++` against stubs, the same as the Pico.
  - The Pico stubs (`pico.h`, `pico/time.h`, `hardware/sync.h`) are replaced by a `platform.h` stub that provides a settable `platform_now_ms()`, no-op critical sections, and `__NOINIT_ATTR` defined as empty.
  - The `tusb.h` stub stays.
- `host_test.cpp` and `log_ring_test.cpp` are copied, with `MAX_KEYBOARDS` now 8.
- The script header of each touched source says to run the test, as on the Pico.
- On-target checks during bring-up (manual):
  - USB enumeration with and without CDC
  - vial.rocks connect and edit
  - 1 to 8 BLE devices, including the ProtoArc XK01, Keychron Nape Pro, Logitech Lift and Keychron M5
  - pairing with passkey
  - `hangtest` then `lastlog`
  - `01-install.sh` on both DevKitC ports, and on the XIAO

### 13.3 Implementation phases

1. **Skeleton.** IDF project, BTstack component, sdkconfig, partitions, scripts. Goal: an empty `btstack_main` reaches `HCI_STATE_WORKING`, and the console prints over UART0.
2. **USB and keymap without BT.** USB descriptors, multiplexer, virtual matrix, VIAL, NVS storage, host tests. Goal: vial.rocks connects and edits persist.
3. **BLE host.** Port `ble_hid_host.cpp` and the `bt_app` messaging. Goal: one keyboard, then 8 devices.
4. **Console, log ring, watchdog, `lastlog`.**
5. **OLED, button.**
6. **Installer, README, udev follow-up, `esp32/README.md`.**

Each phase is built and checked on hardware before the next starts.

---

## 14. Open questions, risks, follow-ups

| # | Item | Default if unanswered |
|---|---|---|
| Q1 | **USB PID.** | **Decided:** `0x4004` (esp_tinyusb's generic HID PID), as a `config.h` constant. |
| Q2 | **Pairing button.** | **Decided:** one external button on `GPIO4` (XIAO `D3`), mounted on the case. BOOT is not used. |
| Q3 | **Extra persistence.** | **Decided:** same as the Pico. Mouse speed and `authreq` stay in RAM. |
| Q4 | **Button reset.** | **Decided:** same as the Pico. The 8 s reset keeps the bindings. |
| R1 | **8 central links plus scanning** on the S3 controller is within spec (9 connections, 10 activities) but untested here. Peripherals asking for 7.5 ms intervals on 8 links may strain scheduling. | Bring-up phase 3 tests 1→8 devices. Without zero latency, if needed. |
| R2 | **(Resolved: works with a real mouse)** **`tud_hid_n_report` from `bt_app`** (another task than `tud_task`) on the DWC2 port. | Fallback `usbd_defer_func` (§6.4). |
| R3 | **(Resolved: verified with `hangtest`)** **`.noinit` survival** across panic and watchdog resets on the S3. | Fallback `RTC_NOINIT_ATTR`, 4 KB (§7.5). |
| R4 | **ROM USB-Serial-JTAG after the forced download boot** (§6.5). | **Resolved** in phase 2: works once the USB PHY is handed back before the restart. Fallback stays BOOT+RESET by hand. |
| R5 | **NVS writes stall the USB interrupt** (not IRAM-safe) for a few ms. | Debounced and per-layer writes. Measure in bring-up. |
| R6 | **HCI receive ring overflow** in the port (drops packets) during scan bursts with 8 links. | Ring sized by `HCI_HOST_ACL_PACKET_NUM`. Drops logged as breadcrumbs. Tune scan duplicate filter. |
| F1 | **udev.** VID `303a` added to `~/cbin/setup/config-hidraw-permission` and its test. | Done. Run the script again to install the rule. |
| F3 | **Pairing method.** The Keychron Nape Pro (on one of its host slots) accepts only LE Secure Connections and drops the link when offered legacy pairing, while the ProtoArc XK01 needs legacy. Both firmwares now flip the method after each failed attempt until a pairing succeeds (also across pairing windows). | Done (Pico and ESP32). |
| F4 | **Device names.** Devices whose advertisement has no name were stored under their address; the ESP32 port reads the GAP Device Name over GATT after the HID service is up. | Done (ESP32 only, by decision). |
| F2 | **Pico fixes.** Pico inconsistencies found during analysis, reported only (no change without approval): keyboard LED reports that arrive on the interrupt OUT endpoint (what Linux uses) still carry the report ID in `buffer[0]`, which the Pico's `tud_hid_set_report_cb` takes as the LED byte; `STATUS_UPDATE_INTERVAL_MS` and `PAIRING_SCAN_TIMEOUT_MS` are unused; the `Pico_2_W` by-id glob in `01-install.sh` can never match; the `00-build.sh` header says the default is pico2_w but it falls back to pico_w; the `crumb_index` sanity check expires after 65520 crumbs. | Report only. |
