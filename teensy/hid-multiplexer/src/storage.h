#ifndef STORAGE_H_
#define STORAGE_H_

#include <stdint.h>
#include <stdbool.h>
#include "config.h"

class StorageManager {
public:
    static void init();
    static bool loadKeymap(uint16_t keymap[NUM_LAYERS][MATRIX_ROWS][MATRIX_COLS]);
    static void saveKeymap(const uint16_t keymap[NUM_LAYERS][MATRIX_ROWS][MATRIX_COLS]);
    static void saveKey(uint8_t layer, uint8_t row, uint8_t col, uint16_t keycode);
    static void resetToDefaults(uint16_t keymap[NUM_LAYERS][MATRIX_ROWS][MATRIX_COLS]);
    static void commit();

private:
    static void writeDefaults();
    static bool isInitialized();
};

#endif // STORAGE_H_
