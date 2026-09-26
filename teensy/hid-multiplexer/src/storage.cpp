#include "storage.h"
#include "logger.h"
#include <EEPROM.h>
#include <Arduino.h>

void StorageManager::init() {
    if (!isInitialized()) {
        logger_println("[Storage] EEPROM not initialized. Writing default keymaps...");
        writeDefaults();
    } else {
        logger_println("[Storage] Valid EEPROM keymap header found.");
    }
}

bool StorageManager::isInitialized() {
    uint32_t magic = 0;
    EEPROM.get(0, magic);
    return (magic == EEPROM_MAGIC);
}

void StorageManager::writeDefaults() {
    uint16_t default_keymap[NUM_LAYERS][MATRIX_ROWS][MATRIX_COLS];
    resetToDefaults(default_keymap);
    saveKeymap(default_keymap);

    uint32_t magic = EEPROM_MAGIC;
    EEPROM.put(0, magic);
}

void StorageManager::resetToDefaults(uint16_t keymap[NUM_LAYERS][MATRIX_ROWS][MATRIX_COLS]) {
    for (uint8_t l = 0; l < NUM_LAYERS; l++) {
        for (uint8_t r = 0; r < MATRIX_ROWS; r++) {
            for (uint8_t c = 0; c < MATRIX_COLS; c++) {
                uint8_t keycode = (r * MATRIX_COLS) + c;
                if (l == 0) {
                    // Layer 0 is 1:1 identity map for all standard keycodes
                    keymap[l][r][c] = keycode;
                } else {
                    // Upper layers default to transparent (0x0000)
                    keymap[l][r][c] = 0x0000;
                }
            }
        }
    }
    // Example default Fn key: Map Right-GUI (keycode 0xE7, row 14, col 7) to MO(1)
    // 0xE7: row = 14, col = 7
    // keymap[0][14][7] = QMK_ACTION_LAYER_MOMENTARY | 1;
}

bool StorageManager::loadKeymap(uint16_t keymap[NUM_LAYERS][MATRIX_ROWS][MATRIX_COLS]) {
    if (!isInitialized()) {
        resetToDefaults(keymap);
        return false;
    }
    int addr = EEPROM_KEYMAP_ADDR;
    for (uint8_t l = 0; l < NUM_LAYERS; l++) {
        for (uint8_t r = 0; r < MATRIX_ROWS; r++) {
            for (uint8_t c = 0; c < MATRIX_COLS; c++) {
                uint16_t val = 0;
                EEPROM.get(addr, val);
                keymap[l][r][c] = val;
                addr += sizeof(uint16_t);
            }
        }
    }
    return true;
}

void StorageManager::saveKeymap(const uint16_t keymap[NUM_LAYERS][MATRIX_ROWS][MATRIX_COLS]) {
    int addr = EEPROM_KEYMAP_ADDR;
    for (uint8_t l = 0; l < NUM_LAYERS; l++) {
        for (uint8_t r = 0; r < MATRIX_ROWS; r++) {
            for (uint8_t c = 0; c < MATRIX_COLS; c++) {
                uint16_t val = keymap[l][r][c];
                EEPROM.put(addr, val);
                addr += sizeof(uint16_t);
            }
        }
    }
    uint32_t magic = EEPROM_MAGIC;
    EEPROM.put(0, magic);
}

void StorageManager::saveKey(uint8_t layer, uint8_t row, uint8_t col, uint16_t keycode) {
    if (layer >= NUM_LAYERS || row >= MATRIX_ROWS || col >= MATRIX_COLS) return;
    int index = (layer * MATRIX_ROWS * MATRIX_COLS) + (row * MATRIX_COLS) + col;
    int addr = EEPROM_KEYMAP_ADDR + (index * sizeof(uint16_t));
    EEPROM.put(addr, keycode);
}

void StorageManager::commit() {
    // Teensy EEPROM emulation automatically commits changed flash pages on put()
}
