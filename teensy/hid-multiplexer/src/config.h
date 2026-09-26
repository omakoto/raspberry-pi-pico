#ifndef CONFIG_H_
#define CONFIG_H_

#include <stdint.h>

// Firmware metadata
#define FW_VERSION_MAJOR 1
#define FW_VERSION_MINOR 0
#define FW_NAME "Teensy HID Multiplexer"

// Downstream USB Host capacity
#define MAX_KEYBOARDS 4
#define MAX_MICE      4
#define MAX_HUBS      2

// Virtual Matrix dimensions for VIAL / VIA
// 16 rows x 16 columns = 256 keyslots (directly maps 0x00 - 0xFF standard USB HID keycodes)
#define MATRIX_ROWS 16
#define MATRIX_COLS 16
#define NUM_LAYERS  4

// Storage Magic & Version (stored in Teensy EEPROM emulation)
#define EEPROM_MAGIC 0x544D5558  // "TMUX"
#define EEPROM_KEYMAP_ADDR 16

// USB Polling & Timing
#define MOUSE_FLUSH_INTERVAL_US 1000  // 1000 Hz mouse output rate
#define LED_HEARTBEAT_INTERVAL_MS 500

// QMK / VIAL Action opcodes
// Basic keycodes: 0x0000 - 0x00FF (standard USB HID usage)
// Layer switching actions:
#define QMK_ACTION_LAYER_MOMENTARY 0x5220  // MO(layer) - bits 0-4 are layer
#define QMK_ACTION_LAYER_TOGGLE    0x5240  // TG(layer) - bits 0-4 are layer
#define QMK_ACTION_LAYER_TO        0x5200  // TO(layer) - bits 0-4 are layer
#define QMK_ACTION_LAYER_ONESHOT   0x5260  // OSL(layer) - bits 0-4 are layer

// Helper macros to test action types
#define IS_ACTION_MO(k)  (((k) & 0xFFE0) == QMK_ACTION_LAYER_MOMENTARY)
#define IS_ACTION_TG(k)  (((k) & 0xFFE0) == QMK_ACTION_LAYER_TOGGLE)
#define IS_ACTION_TO(k)  (((k) & 0xFFE0) == QMK_ACTION_LAYER_TO)
#define IS_ACTION_OSL(k) (((k) & 0xFFE0) == QMK_ACTION_LAYER_ONESHOT)

#define ACTION_LAYER_NUM(k) ((k) & 0x1F)

#endif // CONFIG_H_
