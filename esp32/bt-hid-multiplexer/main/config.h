#ifndef CONFIG_H_
#define CONFIG_H_

#include <stdint.h>

// Every I/O uses a GPIO that is on the XIAO ESP32-S3 header, and the ESP32-S3-DevKitC-1 exposes the
// same GPIOs, so one firmware image runs on both boards.

#define FIRMWARE_VERSION        "1.0.0"

// I2C OLED Display (XIAO D4/D5, the XIAO's default I2C pins)
#define PIN_OLED_SDA            5
#define PIN_OLED_SCL            6
#define OLED_I2C_ADDR           0x3C
#define OLED_WIDTH              128
#define OLED_HEIGHT             64
#define OLED_BAUDRATE_HZ        400000  // SH1106 is specified up to 400 kHz; SSD1306 usually takes 1 MHz

// OLED controller: OLED_SSD1306 (most 0.96" modules) or OLED_SH1106 (most 1.3" modules). The two
// cannot be told apart over I2C, so pick the one on the module.
#define OLED_SSD1306            1
#define OLED_SH1106             2
#define OLED_CONTROLLER         OLED_SH1106

// Push button for BLE pairing (XIAO D3), to GND, internal pull-up
#define PIN_PAIR_BUTTON         4

// The USB serial port is only added to the USB device when this pin (XIAO D8) is connected to GND
// while the board powers up, so that it does not clutter the host with another serial port during
// other projects' development. The UART console is always available.
#define PIN_USB_SERIAL_ENABLE   7

// UART0 console (XIAO D6/D7; on the DevKitC these are wired to the on-board USB-UART bridge)
#define PIN_UART_TX             43
#define PIN_UART_RX             44
#define UART_BAUDRATE           115200

// USB identity. 0x303A is Espressif's vendor ID; 0x4004 is esp_tinyusb's generic HID product ID,
// which other TinyUSB gadgets share, so tools identify this device by its VIAL interface (usage page
// 0xFF60) or its product string as well.
#define USB_VID                 0x303A
#define USB_PID                 0x4004

// Multi-device limits
#define MAX_BLE_DEVICES         8
#define MAX_KEYBOARDS           MAX_BLE_DEVICES
#define MAX_MICE                MAX_BLE_DEVICES

// Virtual Matrix configuration
//
// The matrix is indexed by an 8-bit "virtual key" = row * 16 + col, so one cell exists for every
// possible HID keyboard usage (0x00-0xFF; 0xE0-0xE7 are the modifiers). The otherwise unused
// usages 0xE8-0xFF are used for mouse buttons and mouse motion directions, which lets VIAL remap
// mice exactly like keys.
#define NUM_LAYERS              8
#define MATRIX_ROWS             16
#define MATRIX_COLS             16

// Virtual keys for modifiers (bit n of the HID modifier byte is key VKEY_MODIFIER_BASE + n).
#define VKEY_MODIFIER_BASE      0xE0
// Virtual keys for mouse buttons 1-8 (bit n of the mouse button byte is VKEY_MOUSE_BTN_BASE + n).
// Only buttons 1-5 can be sent to the host (that is all the USB mouse report and the VIAL keycodes
// have), but all 8 can be remapped to other things.
#define VKEY_MOUSE_BTN_BASE     0xE8
#define VKEY_MOUSE_BTN_COUNT    8
#define MOUSE_OUTPUT_BTN_COUNT  5
// Virtual keys for mouse motion directions, in this order.
#define VKEY_MOTION_BASE        0xF0
#define VKEY_MOTION_UP          (VKEY_MOTION_BASE + 0)  // dy < 0
#define VKEY_MOTION_DOWN        (VKEY_MOTION_BASE + 1)  // dy > 0
#define VKEY_MOTION_LEFT        (VKEY_MOTION_BASE + 2)  // dx < 0
#define VKEY_MOTION_RIGHT       (VKEY_MOTION_BASE + 3)  // dx > 0
#define VKEY_WHEEL_UP           (VKEY_MOTION_BASE + 4)  // wheel > 0
#define VKEY_WHEEL_DOWN         (VKEY_MOTION_BASE + 5)  // wheel < 0
#define VKEY_WHEEL_LEFT         (VKEY_MOTION_BASE + 6)  // pan < 0
#define VKEY_WHEEL_RIGHT        (VKEY_MOTION_BASE + 7)  // pan > 0

// Mouse cursor counts that are converted to one wheel notch when a motion direction is remapped to
// a wheel keycode (and the number of counts one wheel notch becomes in the opposite case).
#define MOUSE_COUNTS_PER_WHEEL_NOTCH  24

// QMK keycodes, in the numbering the VIAL client uses at the VIAL protocol version this firmware
// reports (3), i.e. the pre-0.19 QMK numbering: the layer actions and mouse keycodes differ from
// current QMK. Verified against what vial.rocks stores in the keymap (e.g. MO(1) = 0x5101,
// KC_WH_U = 0x00F9, KC_MS_U = 0x00F0 as labelled by the client).
#define KC_NO_                  0x0000
#define KC_F13_                 0x0068  // F13..F15 = defaults of mouse buttons 6-8
#define KC_TRNS_                0x0001  // transparent: use the keycode of the layers below
#define KC_MS_U_                0x00F0  // mouse cursor up/down/left/right
#define KC_MS_D_                0x00F1
#define KC_MS_L_                0x00F2
#define KC_MS_R_                0x00F3
#define KC_BTN1_                0x00F4  // KC_BTN1..KC_BTN5 = mouse button 1..5
#define KC_BTN5_                0x00F8
#define KC_WH_U_                0x00F9  // wheel up/down/left/right
#define KC_WH_D_                0x00FA
#define KC_WH_L_                0x00FB
#define KC_WH_R_                0x00FC
// QMK system/consumer/mouse range (0xA5-0xFF, minus the modifiers) that is not a HID keyboard usage
// and has no USB report here.
#define KC_SPECIAL_FIRST_       0x00A5

// QMK keycodes for the consumer volume keys, sent as the equivalent HID keyboard-page usages
// (Mute 0x7F, Volume Up 0x80, Volume Down 0x81), which the host also understands as volume keys
// and which need no separate consumer report.
#define KC_MUTE_                0x00A8
#define KC_VOLU_                0x00A9
#define KC_VOLD_                0x00AA

// VIAL macros: keycodes M0..M(MACRO_COUNT-1) (QK_MACRO in the VIAL protocol 3 numbering), stored
// as VIA's NUL-separated macro buffer of MACRO_BUFFER_SIZE bytes.
#define MACRO_COUNT             16
#define MACRO_BUFFER_SIZE       1024
#define KC_MACRO_FIRST_         0x5F12
#define IS_MACRO_KEYCODE(k)     ((k) >= KC_MACRO_FIRST_ && (k) < KC_MACRO_FIRST_ + MACRO_COUNT)

// QMK layer actions: the action in bits 8-15, the layer number in bits 0-7.
#define ACTION_LAYER_TO         0x5000 // TO(layer)
#define ACTION_LAYER_MOMENTARY  0x5100 // MO(layer)
#define ACTION_LAYER_DEFAULT    0x5200 // DF(layer)
#define ACTION_LAYER_TOGGLE     0x5300 // TG(layer)

#define IS_ACTION_MO(k)         (((k) & 0xFF00) == ACTION_LAYER_MOMENTARY)
#define IS_ACTION_TG(k)         (((k) & 0xFF00) == ACTION_LAYER_TOGGLE)
#define IS_ACTION_TO(k)         (((k) & 0xFF00) == ACTION_LAYER_TO || ((k) & 0xFF00) == ACTION_LAYER_DEFAULT)
#define ACTION_LAYER_NUM(k)     ((k) & 0xFF)

// QMK modifier-wrapped keycodes (e.g. LSFT(KC_A) = 0x0204): bits 8-11 are LCTL/LSFT/LALT/LGUI,
// bit 12 selects the right-hand modifiers.
#define IS_MODS_KEYCODE(k)      ((k) > 0x00FF && (k) < 0x2000)
#define MODS_KEYCODE_MODS(k)    ((uint8_t)((((k) >> 8) & 0x0F) << (((k) & 0x1000) ? 4 : 0)))

// Persistent storage (NVS namespace of this firmware; BTstack keeps its bonds in "BTstack").
#define NVS_NAMESPACE           "bthidmux"
#define FLASH_BINDINGS_MAGIC    0x42494E44 // 'BIND'
// Bindings of devices that are no longer paired are kept (VIAL lists them, so they can be removed
// there, and a device paired again gets its layer back), up to this many; the ones unpaired longest
// ago are dropped first.
#define MAX_UNPAIRED_BINDINGS   8
// Enough for every paired device (MAX_BLE_DEVICES) plus the unpaired ones.
#define MAX_DEVICE_BINDINGS     (MAX_BLE_DEVICES + MAX_UNPAIRED_BINDINGS)
#define FLASH_KEYMAP_MAGIC      0x5649414C // 'VIAL'
// Bump when the keymap layout (layers, matrix size, keycode meaning) changes; old data is discarded.
#define FLASH_KEYMAP_VERSION    5

// After a peripheral has negotiated its own LL connection parameters and left them alone for a
// while, re-request the same interval with slave latency 0 (see ZERO_LATENCY_DELAY_MS and
// MAX_ZERO_LATENCY_ATTEMPTS in ble_hid_host.cpp).
// HID peripherals ask for latency ~30 to save battery, which lets them skip up to 30 connection
// events; with 0 they answer at every event, which measurably lowers input latency at the cost
// of higher peripheral power draw. Only verified with the Keychron Nape Pro and ProtoArc XK01;
// set to 0 if another device re-negotiates endlessly or drops the link after the update.
#define BLE_ZERO_SLAVE_LATENCY      1

// Whether verbose diagnostics (BTstack's internal log_info lines and advertising reports of
// unrelated devices) are printed on the console from boot. Console output blocks the BTstack
// context and adds input latency, so this is off by default; toggle at runtime with 'log on|off'.
#define BLE_STACK_LOG_DEFAULT       0

// UI and Timer Intervals
#define STATUS_UPDATE_INTERVAL_MS   100
#define PAIRING_SCAN_TIMEOUT_MS     60000

// FreeRTOS task of the BTstack run loop, which owns all application state (DESIGN.md §5). It runs
// on the core of the BT controller task, below the controller's (23) and esp_timer's (22) priority.
#define BT_APP_TASK_CORE        0
#define BT_APP_TASK_PRIORITY    19
#define BT_APP_TASK_STACK_SIZE  8192

#endif // CONFIG_H_
