#ifndef MACROS_H_
#define MACROS_H_

#include <stdint.h>
#include <stddef.h>
#include "config.h"

// The VIAL macro buffer: MACRO_COUNT macros, each terminated by a NUL, in VIA's format (see
// Multiplexer for how they are played). VIAL reads and writes it in pieces; edits are written to
// flash once they have been quiet for a moment, like keymap edits.
class MacroStore {
public:
    static void init();
    static const uint8_t *buffer();
    static void read(uint16_t offset, uint8_t len, uint8_t *out);
    static void write(uint16_t offset, uint8_t len, const uint8_t *data);
    // Clears all macros (VIA "macro reset").
    static void reset();
    // Bounds of macro index (from the buffer start to its NUL); false if there is no such macro.
    static bool find(uint8_t index, const uint8_t **start, const uint8_t **end);
    static void flushPendingSave();

private:
    static uint8_t buffer_[MACRO_BUFFER_SIZE];
    static bool save_pending_;
    static uint32_t last_edit_ms_;
};

#endif // MACROS_H_
