#ifndef DEVICE_BINDINGS_H_
#define DEVICE_BINDINGS_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "config.h"
#include "storage.h"

// Binds individual Bluetooth devices to a keymap layer, so one device can be remapped without
// affecting the others. A bound device's input is looked up on its layer first (see
// VirtualMatrix::resolveAction), so the layer is edited in VIAL like any other. Layer keys pressed on
// a bound device only affect the devices bound to the same layer. Bindings are stored in flash by
// Bluetooth address and so survive reboots and reconnects.
//
// A binding outlives the device's pairing: once a bound device is no longer paired, its binding
// becomes an "unpaired binding", which VIAL lists so it can be removed there, and which applies again
// if the device is paired again with the same address. Only the MAX_UNPAIRED_BINDINGS most recently
// unpaired ones are kept.
class DeviceBindings {
public:
    static const uint8_t NO_LAYER = 0xFF;
    static const uint8_t NO_DEVICE = 0xFF;

    // Looks up the Bluetooth address of a connected device (multiplexer device index); returns false
    // if no device is connected under that index.
    typedef bool (*AddressProvider)(uint8_t dev_idx, uint8_t addr[6]);
    // Returns whether the device with this address is paired, and if so its name (possibly empty).
    typedef bool (*PairedLookup)(const uint8_t addr[6], char *name, size_t name_size);

    static void init(AddressProvider provider, PairedLookup paired);

    // Call once the paired devices are known, and after every change to them (a device paired,
    // unpaired or renamed). Refreshes the names of paired devices, turns the bindings of devices no
    // longer paired into unpaired bindings, and drops the oldest unpaired bindings beyond
    // MAX_UNPAIRED_BINDINGS.
    static void pairingChanged();

    // The layer the device is bound to, or NO_LAYER.
    static uint8_t layerFor(uint8_t dev_idx);

    // Call when the device under an index disconnects or changes.
    static void deviceChanged(uint8_t dev_idx);

    // Remember which device produced input last, so that it can be bound without naming it.
    static void noteActivity(uint8_t dev_idx);
    static uint8_t lastActiveDevice();

    // Layer must be 1..NUM_LAYERS-1. Returns false if the device is not connected, the layer is
    // invalid or the table is full.
    static bool bind(uint8_t dev_idx, uint8_t layer);
    static bool unbind(uint8_t dev_idx);

    // The same by Bluetooth address, for devices that need not be connected (VIAL lists the paired
    // devices and the unpaired bindings). layerForAddress returns NO_LAYER for an unbound address.
    // name is kept for a device that is not paired (when restoring an unpaired binding); a paired
    // device's name comes from the PairedLookup.
    static uint8_t layerForAddress(const uint8_t addr[6]);
    static bool bindAddress(const uint8_t addr[6], uint8_t layer, const char *name = nullptr);
    static bool unbindAddress(const uint8_t addr[6]);
    static void clearAll();

    // Enumeration for the console: stored entries, and the address of a connected device.
    static uint8_t entryCount();
    static bool getEntry(uint8_t index, DeviceBindingEntry *out);
    static bool addressOf(uint8_t dev_idx, uint8_t addr[6]);

    // The unpaired bindings, most recently unpaired first (VIAL's checkboxes).
    static uint8_t unpairedCount();
    static bool getUnpaired(uint8_t index, DeviceBindingEntry *out);
};

#endif // DEVICE_BINDINGS_H_
