#ifndef BINDINGS_FORMAT_H_
#define BINDINGS_FORMAT_H_

#include <stddef.h>
#include <stdint.h>
#include "config.h"
#include "storage.h"

// The stored layout of the device bindings (the "bind" NVS blob), and its conversion from older
// versions. Kept apart from StorageManager, which does the NVS access, so that the host tests can
// run it (test/bindings_format_test.cpp). Run test/run-host-test.sh after changing this file or
// bindings_format.cpp.

struct BindingsBlob {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    DeviceBindingEntry entries[MAX_DEVICE_BINDINGS];
};

static const uint16_t BINDINGS_VERSION = 2;

// Version 1: 8 entries of address, layer and used, without names. Read once and converted.
struct BindingEntryV1 {
    uint8_t addr[6];
    uint8_t layer;
    uint8_t used;
};
struct BindingsBlobV1 {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    BindingEntryV1 entries[8];
};
static_assert(MAX_DEVICE_BINDINGS >= 8, "version 1 bindings must fit");

// Stored data must keep loading, so the layouts are pinned.
static_assert(sizeof(DeviceBindingEntry) == 44, "stored layout changed");
static_assert(sizeof(BindingsBlobV1) == 72, "stored layout changed");
static_assert(MAX_DEVICE_BINDINGS != 16 || sizeof(BindingsBlob) == 712, "stored layout changed");

// Decodes a stored blob of len bytes into entries. Returns false if it is not a bindings blob of a
// known version. needs_save is set when it was in an older version, converted, and should be stored
// again; the names and the unpaired order of converted entries are left for
// DeviceBindings::pairingChanged() to fill in once the paired devices are known.
bool bindings_decode(const void *data, size_t len, DeviceBindingEntry entries[MAX_DEVICE_BINDINGS],
                     bool &needs_save);

// The blob to store for entries, in the current version.
void bindings_encode(const DeviceBindingEntry entries[MAX_DEVICE_BINDINGS], BindingsBlob *out);

#endif // BINDINGS_FORMAT_H_
