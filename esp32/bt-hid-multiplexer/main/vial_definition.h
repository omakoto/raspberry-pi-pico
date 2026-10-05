#ifndef VIAL_DEFINITION_H_
#define VIAL_DEFINITION_H_

#include <stdint.h>
#include <stddef.h>

// The VIAL keyboard definition is built at runtime so that it can carry the bonded Bluetooth
// devices: each one gets a "layout option" dropdown in VIAL's Layout tab, labelled with the device
// name, with the choices "No binding" and "Layer 1".."Layer 7". VIAL reads and writes the dropdowns
// as one 32-bit "layout options" value (VIA keyboard value 0x02). This is how a device is bound to a
// keymap layer without the console (see DeviceBindings).

// Devices listed in the definition, in dropdown order.
#define VIAL_MAX_DEVICE_OPTIONS 8
// Bits per dropdown: 8 choices ("No binding" = 0, layer N = N).
#define VIAL_OPTION_BITS 3

struct VialDeviceEntry {
    uint8_t addr[6];
    char name[32];
};

// Builds the definition (JSON with one dropdown per device) and wraps it in an .xz stream, which is
// what VIAL expects. Returns the size written to out, or 0 if out is too small.
size_t vial_build_definition(const VialDeviceEntry *devices, uint8_t count, uint8_t *out, size_t out_size);

// Wraps data in a valid .xz stream without compressing it (LZMA2 "uncompressed" chunks, CRC32
// check). Returns the size written to out, or 0 if out is too small.
size_t xz_wrap_uncompressed(const uint8_t *data, size_t len, uint8_t *out, size_t out_size);

// The 32-bit layout options value for count dropdowns and back, in VIAL's packing: the dropdowns'
// bits are concatenated in order, the first dropdown in the most significant position.
uint32_t vial_pack_layout_options(const uint8_t *choices, uint8_t count);
void vial_unpack_layout_options(uint32_t value, uint8_t count, uint8_t *choices);

#endif // VIAL_DEFINITION_H_
