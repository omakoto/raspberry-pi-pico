#include "device_bindings.h"
#include "storage.h"
#include <stdio.h>
#include <string.h>

static DeviceBindingEntry s_entries[MAX_DEVICE_BINDINGS];
static DeviceBindings::AddressProvider s_provider = nullptr;
static DeviceBindings::PairedLookup s_paired = nullptr;
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

void DeviceBindings::init(AddressProvider provider, PairedLookup paired) {
    s_provider = provider;
    s_paired = paired;
    s_last_active = NO_DEVICE;
    memset(s_entries, 0, sizeof(s_entries));
    bool needs_save = false;
    if (!StorageManager::loadBindings(s_entries, needs_save)) {
        memset(s_entries, 0, sizeof(s_entries));
    } else if (needs_save) {
        StorageManager::saveBindings(s_entries);
    }
    invalidate_cache();
}

static uint8_t count_unpaired() {
    uint8_t n = 0;
    for (int i = 0; i < MAX_DEVICE_BINDINGS; i++) {
        if (s_entries[i].used && s_entries[i].unpaired_seq != 0) n++;
    }
    return n;
}

// Brings the entries in line with the paired devices (see pairingChanged()); returns whether any
// entry changed.
static bool refresh_pairing() {
    if (s_paired == nullptr) return false;
    bool changed = false;
    uint32_t next_seq = 1;
    for (int i = 0; i < MAX_DEVICE_BINDINGS; i++) {
        if (s_entries[i].used && s_entries[i].unpaired_seq >= next_seq) next_seq = s_entries[i].unpaired_seq + 1;
    }
    for (int i = 0; i < MAX_DEVICE_BINDINGS; i++) {
        DeviceBindingEntry &e = s_entries[i];
        if (!e.used) continue;
        char name[sizeof(e.name)];
        name[0] = '\0';
        if (s_paired(e.addr, name, sizeof(name))) {
            if (e.unpaired_seq != 0) {
                e.unpaired_seq = 0;
                changed = true;
            }
            name[sizeof(name) - 1] = '\0';
            if (name[0] != '\0' && strcmp(name, e.name) != 0) {
                memcpy(e.name, name, sizeof(e.name));
                changed = true;
            }
        } else if (e.unpaired_seq == 0) {
            e.unpaired_seq = next_seq++;
            changed = true;
        }
    }
    while (count_unpaired() > MAX_UNPAIRED_BINDINGS) {
        int oldest = -1;
        for (int i = 0; i < MAX_DEVICE_BINDINGS; i++) {
            const DeviceBindingEntry &e = s_entries[i];
            if (!e.used || e.unpaired_seq == 0) continue;
            if (oldest < 0 || e.unpaired_seq < s_entries[oldest].unpaired_seq) oldest = i;
        }
        memset(&s_entries[oldest], 0, sizeof(s_entries[oldest]));
        changed = true;
    }
    return changed;
}

void DeviceBindings::pairingChanged() {
    if (refresh_pairing()) {
        StorageManager::saveBindings(s_entries);
        invalidate_cache();
    }
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
    uint8_t addr[6];
    if (!addressOf(dev_idx, addr)) return false;
    return bindAddress(addr, layer);
}

bool DeviceBindings::unbind(uint8_t dev_idx) {
    uint8_t addr[6];
    if (!addressOf(dev_idx, addr)) return false;
    return unbindAddress(addr);
}

uint8_t DeviceBindings::layerForAddress(const uint8_t addr[6]) {
    int e = find_entry(addr);
    return (e >= 0) ? s_entries[e].layer : NO_LAYER;
}

bool DeviceBindings::bindAddress(const uint8_t addr[6], uint8_t layer, const char *name) {
    if (layer < 1 || layer >= NUM_LAYERS) return false;
    int e = find_entry(addr);
    if (e < 0) {
        for (int i = 0; i < MAX_DEVICE_BINDINGS; i++) {
            if (!s_entries[i].used) { e = i; break; }
        }
        if (e < 0) return false;
        memset(&s_entries[e], 0, sizeof(s_entries[e]));
        memcpy(s_entries[e].addr, addr, 6);
        s_entries[e].used = 1;
    }
    s_entries[e].layer = layer;
    if (name != nullptr && name[0] != '\0') {
        snprintf(s_entries[e].name, sizeof(s_entries[e].name), "%s", name);
    }
    // A new entry takes the paired device's name, or becomes an unpaired binding at once.
    refresh_pairing();
    StorageManager::saveBindings(s_entries);
    invalidate_cache();
    return true;
}

bool DeviceBindings::unbindAddress(const uint8_t addr[6]) {
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

bool DeviceBindings::getEntry(uint8_t index, DeviceBindingEntry *out) {
    uint8_t n = 0;
    for (int i = 0; i < MAX_DEVICE_BINDINGS; i++) {
        if (!s_entries[i].used) continue;
        if (n++ == index) {
            *out = s_entries[i];
            return true;
        }
    }
    return false;
}

uint8_t DeviceBindings::unpairedCount() {
    return count_unpaired();
}

bool DeviceBindings::getUnpaired(uint8_t index, DeviceBindingEntry *out) {
    // The index-th largest unpaired_seq. The table is small, so this just counts the entries that
    // sort before each one.
    for (int i = 0; i < MAX_DEVICE_BINDINGS; i++) {
        const DeviceBindingEntry &e = s_entries[i];
        if (!e.used || e.unpaired_seq == 0) continue;
        uint8_t newer = 0;
        for (int j = 0; j < MAX_DEVICE_BINDINGS; j++) {
            if (s_entries[j].used && s_entries[j].unpaired_seq > e.unpaired_seq) newer++;
        }
        if (newer == index) {
            *out = e;
            return true;
        }
    }
    return false;
}
