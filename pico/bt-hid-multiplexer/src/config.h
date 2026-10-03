#ifndef CONFIG_H_
#define CONFIG_H_

#include <stdint.h>

// I2C OLED Display (matches circuitpython/ssd1306 spec)
#define I2C_PORT_OLED           i2c1
#define PIN_OLED_SDA            2
#define PIN_OLED_SCL            3
#define OLED_I2C_ADDR           0x3C
#define OLED_WIDTH              128
#define OLED_HEIGHT             64
#define OLED_BAUDRATE_HZ        400000

// Push button for BLE pairing and status toggle
#define PIN_PAIR_BUTTON         6

// Hardware UART0 Console (matches nsbackend-pico pinout)
#define UART_PORT               uart0
#define PIN_UART_TX             16 // GP16 (Physical Pin 21)
#define PIN_UART_RX             17 // GP17 (Physical Pin 22)
#define UART_BAUDRATE           115200

// Multi-device limits
#define MAX_BLE_DEVICES         8
#define MAX_KEYBOARDS           8
#define MAX_MICE                8

// Virtual Matrix configuration
#define NUM_LAYERS              4
#define MATRIX_ROWS             4
#define MATRIX_COLS             16

// QMK / VIA Layer Action Macros
#define ACTION_LAYER_MOMENTARY  0x5200 // MO(layer)
#define ACTION_LAYER_TOGGLE     0x5220 // TG(layer)
#define ACTION_LAYER_TO         0x5240 // TO(layer)

#define IS_ACTION_MO(k)         (((k) & 0xFFE0) == ACTION_LAYER_MOMENTARY)
#define IS_ACTION_TG(k)         (((k) & 0xFFE0) == ACTION_LAYER_TOGGLE)
#define IS_ACTION_TO(k)         (((k) & 0xFFE0) == ACTION_LAYER_TO)
#define ACTION_LAYER_NUM(k)     ((k) & 0x1F)

// Flash storage offsets for keymap persistence (placed safely 64KB before end of flash)
#define FLASH_KEYMAP_OFFSET     (PICO_FLASH_SIZE_BYTES - (64 * 1024))
#define FLASH_KEYMAP_MAGIC      0x5649414C // 'VIAL'

// After a peripheral has negotiated its own LL connection parameters, re-request the same
// interval with slave latency 0 (once per connection). HID peripherals ask for latency ~30 to
// save battery, which lets them skip up to 30 connection events; with 0 they answer at every
// event, which measurably lowers input latency at the cost of higher peripheral power draw.
#define BLE_ZERO_SLAVE_LATENCY      1

// Whether verbose diagnostics (BTstack's internal log_info lines and advertising reports of
// unrelated devices) are printed on the console from boot. Console output blocks the BTstack
// context and adds input latency, so this is off by default; toggle at runtime with 'log on|off'.
#define BLE_STACK_LOG_DEFAULT       0

// UI and Timer Intervals
#define STATUS_UPDATE_INTERVAL_MS   100
#define HEARTBEAT_INTERVAL_MS       500
#define PAIRING_SCAN_TIMEOUT_MS     60000

#endif // CONFIG_H_
