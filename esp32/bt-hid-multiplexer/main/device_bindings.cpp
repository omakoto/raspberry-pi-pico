#include "device_bindings.h"
#include "storage.h"
#include <string.h>

static DeviceBindingEntry s_entries[MAX_DEVICE_BINDINGS];
static DeviceBindings::AddressProvider s_provider = nullptr;
static uint8_t s_last_active = DeviceBindings::NO_DEVICE;

// Resolved layer per device index; only valid where s_cache_valid is set, because the address of a
// device that has not finished connecting cannot be looked up yet.
static uint8_t s_cache[MAX_KEYBOARDS];
static bool s_cache_valid[MAX_KEYBOARDS];

static int find_entry(const uint8_t addr[6]) {
    for (int i = 0; i < MAX_DEVICE_BINDINGS; i++) {
        if (s_entries[i].used && memcmp(s_entries[i].addr, addr, 6) == 0) return i;
    }
    return -1;
}

static void invalidate_cache() {
    memset(s_cache_valid, 0, sizeof(s_cache_valid));
}

void DeviceBindings::init(AddressProvider provider) {
    s_provider = provider;
    s_last_active = NO_DEVICE;
    memset(s_entries, 0, sizeof(s_entries));
    StorageManager::loadBindings(s_entries);
    invalidate_cache();
}

bool DeviceBindings::addressOf(uint8_t dev_idx, uint8_t addr[6]) {
    return s_provider != nullptr && dev_idx < MAX_KEYBOARDS && s_provider(dev_idx, addr);
}

uint8_t DeviceBindings::layerFor(uint8_t dev_idx) {
    if (dev_idx >= MAX_KEYBOARDS) return NO_LAYER;
    if (s_cache_valid[dev_idx]) return s_cache[dev_idx];

    uint8_t addr[6];
    if (!addressOf(dev_idx, addr)) return NO_LAYER;
    int e = find_entry(addr);
    s_cache[dev_idx] = (e >= 0) ? s_entries[e].layer : NO_LAYER;
    s_cache_valid[dev_idx] = true;
    return s_cache[dev_idx];
}

void DeviceBindings::deviceChanged(uint8_t dev_idx) {
    if (dev_idx < MAX_KEYBOARDS) s_cache_valid[dev_idx] = false;
    if (s_last_active == dev_idx) s_last_active = NO_DEVICE;
}

void DeviceBindings::noteActivity(uint8_t dev_idx) {
    if (dev_idx < MAX_KEYBOARDS) s_last_active = dev_idx;
}

uint8_t DeviceBindings::lastActiveDevice() {
    return s_last_active;
}

bool DeviceBindings::bind(uint8_t dev_idx, uint8_t layer) {
    if (layer < 1 || layer >= NUM_LAYERS) return false;
    uint8_t addr[6];
    if (!addressOf(dev_idx, addr)) return false;

    int e = find_entry(addr);
    if (e < 0) {
        for (int i = 0; i < MAX_DEVICE_BINDINGS; i++) {
            if (!s_entries[i].used) { e = i; break; }
        }
        if (e < 0) return false;
        memcpy(s_entries[e].addr, addr, 6);
        s_entries[e].used = 1;
    }
    s_entries[e].layer = layer;
    StorageManager::saveBindings(s_entries);
    invalidate_cache();
    return true;
}

bool DeviceBindings::unbind(uint8_t dev_idx) {
    uint8_t addr[6];
    if (!addressOf(dev_idx, addr)) return false;
    int e = find_entry(addr);
    if (e < 0) return false;
    memset(&s_entries[e], 0, sizeof(s_entries[e]));
    StorageManager::saveBindings(s_entries);
    invalidate_cache();
    return true;
}

void DeviceBindings::clearAll() {
    memset(s_entries, 0, sizeof(s_entries));
    StorageManager::saveBindings(s_entries);
    invalidate_cache();
}

uint8_t DeviceBindings::entryCount() {
    uint8_t n = 0;
    for (int i = 0; i < MAX_DEVICE_BINDINGS; i++) {
        if (s_entries[i].used) n++;
    }
    return n;
}

bool DeviceBindings::getEntry(uint8_t index, uint8_t addr[6], uint8_t *layer) {
    uint8_t n = 0;
    for (int i = 0; i < MAX_DEVICE_BINDINGS; i++) {
        if (!s_entries[i].used) continue;
        if (n++ == index) {
            memcpy(addr, s_entries[i].addr, 6);
            *layer = s_entries[i].layer;
            return true;
        }
    }
    return false;
}
