#include "bindings_format.h"
#include <stddef.h>
#include <string.h>

bool bindings_decode(const void *data, size_t len, DeviceBindingEntry entries[MAX_DEVICE_BINDINGS],
                     bool &needs_save) {
    needs_save = false;
    if (len == sizeof(BindingsBlob)) {
        const uint8_t *bytes = (const uint8_t *)data;
        uint32_t magic;
        uint16_t version;
        memcpy(&magic, bytes + offsetof(BindingsBlob, magic), sizeof(magic));
        memcpy(&version, bytes + offsetof(BindingsBlob, version), sizeof(version));
        if (magic != FLASH_BINDINGS_MAGIC || version != BINDINGS_VERSION) return false;
        memcpy(entries, bytes + offsetof(BindingsBlob, entries), sizeof(DeviceBindingEntry) * MAX_DEVICE_BINDINGS);
        return true;
    }
    if (len == sizeof(BindingsBlobV1)) {
        BindingsBlobV1 v1;
        memcpy(&v1, data, sizeof(v1));
        if (v1.magic != FLASH_BINDINGS_MAGIC || v1.version != 1) return false;
        memset(entries, 0, sizeof(DeviceBindingEntry) * MAX_DEVICE_BINDINGS);
        for (int i = 0; i < 8; i++) {
            memcpy(entries[i].addr, v1.entries[i].addr, sizeof(entries[i].addr));
            entries[i].layer = v1.entries[i].layer;
            entries[i].used = v1.entries[i].used;
        }
        needs_save = true;
        return true;
    }
    return false;
}

void bindings_encode(const DeviceBindingEntry entries[MAX_DEVICE_BINDINGS], BindingsBlob *out) {
    memset(out, 0, sizeof(*out));
    out->magic = FLASH_BINDINGS_MAGIC;
    out->version = BINDINGS_VERSION;
    memcpy(out->entries, entries, sizeof(out->entries));
}
