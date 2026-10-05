#ifndef CONFIG_H_
#define CONFIG_H_

#include <stdint.h>

// Board selection (./00-build.sh -b <devkitc|xiao>). Every external I/O uses a GPIO that is on the
// XIAO ESP32-S3 header, and the DevKitC exposes the same GPIOs, so only the on-board LED differs.
#if defined(BOARD_XIAO)
#define BOARD_NAME              "XIAO ESP32-S3"
#elif defined(BOARD_DEVKITC)
#define BOARD_NAME              "ESP32-S3-DevKitC-1"
#else
#error "Define BOARD_XIAO or BOARD_DEVKITC (see CMakeLists.txt)"
#endif

#define FIRMWARE_VERSION        "1.0.0"

// I2C OLED Display (XIAO D4/D5, the XIAO's default I2C pins)
#define PIN_OLED_SDA            5
#define PIN_OLED_SCL            6
#define OLED_I2C_ADDR           0x3C
#define OLED_WIDTH              128
#define OLED_HEIGHT             64
#define OLED_BAUDRATE_HZ        400000

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

// On-board pairing LED
#if defined(BOARD_XIAO)
#define PIN_LED                 21   // yellow user LED, active LOW
#else
// WS2812 RGB LED: GPIO48 on DevKitC v1.0, GPIO38 on v1.1. Both are driven so that either works.
#define PIN_RGB_LED_V1_0        48
#define PIN_RGB_LED_V1_1        38
#endif

// FreeRTOS task of the BTstack run loop, which owns all application state (DESIGN.md §5). It runs
// on the core of the BT controller task, below the controller's (23) and esp_timer's (22) priority.
#define BT_APP_TASK_CORE        0
#define BT_APP_TASK_PRIORITY    19
#define BT_APP_TASK_STACK_SIZE  8192

#endif // CONFIG_H_
