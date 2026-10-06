#ifndef DEVICE_BINDINGS_H_
#define DEVICE_BINDINGS_H_

#include <stdint.h>
#include <stdbool.h>
#include "config.h"

// Binds individual Bluetooth devices to a keymap layer, so one device can be remapped without
// affecting the others. A bound device's input is looked up on its layer first (see
// VirtualMatrix::resolveAction), so the layer is edited in VIAL like any other. Layer keys pressed on
// a bound device only affect the devices bound to the same layer. Bindings are stored in flash by
// Bluetooth address and so survive reboots and reconnects.
class DeviceBindings {
public:
    static const uint8_t NO_LAYER = 0xFF;
    static const uint8_t NO_DEVICE = 0xFF;

    // Looks up the Bluetooth address of a connected device (multiplexer device index); returns false
    // if no device is connected under that index.
    typedef bool (*AddressProvider)(uint8_t dev_idx, uint8_t addr[6]);

    static void init(AddressProvider provider);

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

    // The same by Bluetooth address, for devices that need not be connected (VIAL lists the bonded
    // devices). layerForAddress returns NO_LAYER for an unbound address.
    static uint8_t layerForAddress(const uint8_t addr[6]);
    static bool bindAddress(const uint8_t addr[6], uint8_t layer);
    static bool unbindAddress(const uint8_t addr[6]);
    static void clearAll();

    // Enumeration for the console: stored entries, and the address of a connected device.
    static uint8_t entryCount();
    static bool getEntry(uint8_t index, uint8_t addr[6], uint8_t *layer);
    static bool addressOf(uint8_t dev_idx, uint8_t addr[6]);
};

#endif // DEVICE_BINDINGS_H_
