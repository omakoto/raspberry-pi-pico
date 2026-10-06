#ifndef VIAL_DEFINITION_H_
#define VIAL_DEFINITION_H_

#include <stdint.h>
#include <stddef.h>

// The VIAL keyboard definition is built at runtime so that it can carry the bonded Bluetooth
// devices: each one gets a "layout option" dropdown in VIAL's Layout tab, labelled with the device
// name, with the choices "No binding" and "Layer 1".."Layer 7". This is how a device is bound to a
// keymap layer without the console (see DeviceBindings). After the dropdowns, each binding of a
// device that is no longer paired gets a checkbox ("Unpaired: <name> (layer N)"), checked while the
// binding exists, so it can be removed.
//
// VIAL reads and writes all of them as one 32-bit "layout options" value (VIA keyboard value 0x02),
// with as many bits per option as its choices need: 3 per dropdown and 1 per checkbox. 8 dropdowns
// and 8 checkboxes use all 32 bits.

// Paired devices listed in the definition, in dropdown order.
#define VIAL_MAX_DEVICE_OPTIONS 8
// Bits per dropdown: 8 choices ("No binding" = 0, layer N = N).
#define VIAL_OPTION_BITS 3
// Unpaired bindings listed in the definition, in checkbox order, after the dropdowns.
#define VIAL_MAX_UNPAIRED_OPTIONS 8
static_assert(VIAL_MAX_DEVICE_OPTIONS * VIAL_OPTION_BITS + VIAL_MAX_UNPAIRED_OPTIONS <= 32,
              "the layout options must fit VIA's 32-bit value");

struct VialDeviceEntry {
    uint8_t addr[6];
    char name[32];
    uint8_t layer;  // unpaired bindings only: the layer the device is bound to
};

// Builds the definition (JSON with one dropdown per paired device and one checkbox per unpaired
// binding) and wraps it in an .xz stream, which is what VIAL expects. Returns the size written to
// out, or 0 if out is too small.
size_t vial_build_definition(const VialDeviceEntry *devices, uint8_t count,
                             const VialDeviceEntry *unpaired, uint8_t unpaired_count,
                             uint8_t *out, size_t out_size);

// Wraps data in a valid .xz stream without compressing it (LZMA2 "uncompressed" chunks, CRC32
// check). Returns the size written to out, or 0 if out is too small.
size_t xz_wrap_uncompressed(const uint8_t *data, size_t len, uint8_t *out, size_t out_size);

// The 32-bit layout options value for count dropdowns followed by checked_count checkboxes, and
// back, in VIAL's packing: the options' bits are concatenated in order, the first option in the most
// significant position.
uint32_t vial_pack_layout_options(const uint8_t *choices, uint8_t count,
                                  const bool *checked, uint8_t checked_count);
void vial_unpack_layout_options(uint32_t value, uint8_t count, uint8_t *choices,
                                uint8_t checked_count, bool *checked);

#endif // VIAL_DEFINITION_H_
