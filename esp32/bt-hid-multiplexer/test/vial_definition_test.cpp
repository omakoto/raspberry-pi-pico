// Host-side test of main/vial_definition.cpp: the layout option packing (which must match VIAL's),
// and the runtime keyboard definition. The definitions' .xz streams are written to the files given as
// the arguments (a typical one, then the largest one); run-host-test.sh checks them with Python's
// lzma and json modules, which is what VIAL itself uses to read them.
#include <stdio.h>
#include <string.h>
#include "vial_definition.h"

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)

int main(int argc, char **argv) {
    // VIAL concatenates the options' bits, the first option most significant: dropdowns 3, 0, 7
    // and checkboxes on, off -> 011 000 111 1 0.
    uint8_t choices[3] = {3, 0, 7};
    CHECK(vial_pack_layout_options(choices, 3, nullptr, 0) == 0b011000111);
    bool checked[2] = {true, false};
    CHECK(vial_pack_layout_options(choices, 3, checked, 2) == 0b01100011110);
    uint8_t back[3] = {9, 9, 9};
    bool checked_back[2] = {false, true};
    vial_unpack_layout_options(0b01100011110, 3, back, 2, checked_back);
    CHECK(back[0] == 3 && back[1] == 0 && back[2] == 7);
    CHECK(checked_back[0] && !checked_back[1]);
    CHECK(vial_pack_layout_options(choices, 0, nullptr, 0) == 0);
    // 8 dropdowns and 8 checkboxes use all 32 bits.
    uint8_t eight[8] = {1, 2, 3, 4, 5, 6, 7, 0};
    bool flags[8] = {true, false, true, true, false, false, true, true};
    uint32_t full = vial_pack_layout_options(eight, 8, flags, 8);
    CHECK((full >> 29) == 1 && (full & 0xFF) == 0b10110011);
    uint8_t eight_back[8];
    bool flags_back[8];
    vial_unpack_layout_options(full, 8, eight_back, 8, flags_back);
    CHECK(memcmp(eight, eight_back, 8) == 0 && memcmp(flags, flags_back, sizeof(flags)) == 0);
    // Checkboxes alone.
    vial_unpack_layout_options(0b101, 0, nullptr, 3, flags_back);
    CHECK(flags_back[0] && !flags_back[1] && flags_back[2]);

    // Too small an output buffer is refused rather than overrun.
    uint8_t tiny[16];
    CHECK(xz_wrap_uncompressed((const uint8_t *)"hello", 5, tiny, sizeof(tiny)) == 0);

    // A definition with names that need escaping or cleaning, and one without a name. Two devices
    // with the same name get the end of their addresses added, also when one of them is unpaired.
    VialDeviceEntry devices[5] = {};
    strcpy(devices[0].name, "Keychron Nape Pro");
    strcpy(devices[1].name, "Quote\" Back\\slash \xE2\x80\x99");
    memcpy(devices[2].addr, "\xD6\x54\xCB\x91\x59\x74", 6);
    strcpy(devices[3].name, "MX Dialpad");
    memcpy(devices[3].addr, "\xD3\x33\x9A\x41\x7B\x44", 6);
    strcpy(devices[4].name, "MX Dialpad");
    memcpy(devices[4].addr, "\xC1\x02\x03\x04\x05\x06", 6);
    VialDeviceEntry unpaired[2] = {};
    strcpy(unpaired[0].name, "Keychron Nape Pro");
    memcpy(unpaired[0].addr, "\xE0\x01\x02\x03\xAB\xCD", 6);
    unpaired[0].layer = 3;
    memcpy(unpaired[1].addr, "\xE1\x02\x03\x04\x05\x07", 6);
    unpaired[1].layer = 7;
    static uint8_t def[6400];
    size_t size = vial_build_definition(devices, 5, unpaired, 2, def, sizeof(def));
    CHECK(size > 0);
    if (argc > 1 && size > 0) {
        FILE *f = fopen(argv[1], "wb");
        fwrite(def, 1, size, f);
        fclose(f);
    }

    // The largest definition, 8 devices and 8 unpaired bindings with the longest names, fits the
    // firmware's buffer (vial_server.cpp).
    VialDeviceEntry many[8] = {}, many_unpaired[8] = {};
    for (int i = 0; i < 8; i++) {
        memset(many[i].name, 'x', sizeof(many[i].name) - 1);
        memset(many_unpaired[i].name, 'y', sizeof(many_unpaired[i].name) - 1);
        many[i].addr[5] = (uint8_t)i;
        many_unpaired[i].addr[5] = (uint8_t)(0x10 + i);
        many_unpaired[i].layer = 7;
    }
    size = vial_build_definition(many, 8, many_unpaired, 8, def, sizeof(def));
    CHECK(size > 0);
    if (argc > 2 && size > 0) {
        FILE *f = fopen(argv[2], "wb");
        fwrite(def, 1, size, f);
        fclose(f);
    }

    if (g_failures) { printf("%d FAILURES\n", g_failures); return 1; }
    printf("All vial definition tests passed\n");
    return 0;
}
