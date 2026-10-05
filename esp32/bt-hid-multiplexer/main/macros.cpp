#include "macros.h"
#include "platform.h"
#include "storage.h"

#include <string.h>

uint8_t MacroStore::buffer_[MACRO_BUFFER_SIZE];
bool MacroStore::save_pending_ = false;
uint32_t MacroStore::last_edit_ms_ = 0;

// How long macro edits must be quiet before they are written to flash. VIAL writes the buffer in
// 28-byte pieces.
static const uint32_t SAVE_DELAY_MS = 500;

void MacroStore::init() {
    save_pending_ = false;
    if (!StorageManager::loadMacros(buffer_)) {
        memset(buffer_, 0, sizeof(buffer_));
    }
}

const uint8_t *MacroStore::buffer() {
    return buffer_;
}

void MacroStore::read(uint16_t offset, uint8_t len, uint8_t *out) {
    for (uint8_t i = 0; i < len; i++) {
        uint32_t pos = (uint32_t)offset + i;
        out[i] = (pos < sizeof(buffer_)) ? buffer_[pos] : 0;
    }
}

void MacroStore::write(uint16_t offset, uint8_t len, const uint8_t *data) {
    for (uint8_t i = 0; i < len; i++) {
        uint32_t pos = (uint32_t)offset + i;
        if (pos < sizeof(buffer_)) buffer_[pos] = data[i];
    }
    save_pending_ = true;
    last_edit_ms_ = platform_now_ms();
}

void MacroStore::reset() {
    memset(buffer_, 0, sizeof(buffer_));
    save_pending_ = false;
    StorageManager::saveMacros(buffer_);
}

bool MacroStore::find(uint8_t index, const uint8_t **start, const uint8_t **end) {
    const uint8_t *p = buffer_;
    const uint8_t *limit = buffer_ + sizeof(buffer_);
    for (uint8_t i = 0; i < index; i++) {
        p = (const uint8_t *)memchr(p, 0, limit - p);
        if (!p) return false;
        p++;
    }
    const uint8_t *nul = (const uint8_t *)memchr(p, 0, limit - p);
    *start = p;
    *end = nul ? nul : limit;
    return true;
}

void MacroStore::flushPendingSave() {
    if (!save_pending_) return;
    if (platform_now_ms() - last_edit_ms_ < SAVE_DELAY_MS) return;
    save_pending_ = false;
    StorageManager::saveMacros(buffer_);
}
