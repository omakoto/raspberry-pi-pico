#ifndef STORAGE_H_
#define STORAGE_H_

#include <stdint.h>
#include <stdbool.h>
#include "config.h"

struct KeymapStorageData {
    uint32_t magic;
    uint16_t version;
    uint16_t checksum;
    uint16_t keymap[NUM_LAYERS][MATRIX_ROWS][MATRIX_COLS];
};

class StorageManager {
public:
    static void init();
    static bool loadKeymap(uint16_t keymap[NUM_LAYERS][MATRIX_ROWS][MATRIX_COLS]);
    static void saveKeymap(const uint16_t keymap[NUM_LAYERS][MATRIX_ROWS][MATRIX_COLS]);
    static void clearKeymap();

private:
    static uint16_t computeChecksum(const uint16_t keymap[NUM_LAYERS][MATRIX_ROWS][MATRIX_COLS]);
};

#endif // STORAGE_H_
