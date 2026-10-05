// Host-side test of main/vial_definition.cpp: the layout option packing (which must match VIAL's),
// and the runtime keyboard definition. The definition's .xz stream is written to the file given as
// the first argument; run-host-test.sh checks it with Python's lzma and json modules, which is what
// VIAL itself uses to read it.
#include <stdio.h>
#include <string.h>
#include "vial_definition.h"

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)

int main(int argc, char **argv) {
    // VIAL concatenates the dropdowns' bits, the first dropdown most significant:
    // choices 3, 0, 7 -> 011 000 111.
    uint8_t choices[3] = {3, 0, 7};
    CHECK(vial_pack_layout_options(choices, 3) == 0b011000111);
    uint8_t back[3] = {9, 9, 9};
    vial_unpack_layout_options(0b011000111, 3, back);
    CHECK(back[0] == 3 && back[1] == 0 && back[2] == 7);
    uint8_t eight[8] = {1, 2, 3, 4, 5, 6, 7, 0};
    uint8_t eight_back[8];
    vial_unpack_layout_options(vial_pack_layout_options(eight, 8), 8, eight_back);
    CHECK(memcmp(eight, eight_back, 8) == 0);
    CHECK(vial_pack_layout_options(choices, 0) == 0);

    // Too small an output buffer is refused rather than overrun.
    uint8_t tiny[16];
    CHECK(xz_wrap_uncompressed((const uint8_t *)"hello", 5, tiny, sizeof(tiny)) == 0);

    // A definition with names that need escaping or cleaning, and one without a name.
    // Two devices with the same name get the end of their addresses added.
    VialDeviceEntry devices[5] = {};
    strcpy(devices[0].name, "Keychron Nape Pro");
    strcpy(devices[1].name, "Quote\" Back\\slash \xE2\x80\x99");
    memcpy(devices[2].addr, "\xD6\x54\xCB\x91\x59\x74", 6);
    strcpy(devices[3].name, "MX Dialpad");
    memcpy(devices[3].addr, "\xD3\x33\x9A\x41\x7B\x44", 6);
    strcpy(devices[4].name, "MX Dialpad");
    memcpy(devices[4].addr, "\xC1\x02\x03\x04\x05\x06", 6);
    static uint8_t def[8192];
    size_t size = vial_build_definition(devices, 5, def, sizeof(def));
    CHECK(size > 0);
    if (argc > 1 && size > 0) {
        FILE *f = fopen(argv[1], "wb");
        fwrite(def, 1, size, f);
        fclose(f);
    }

    if (g_failures) { printf("%d FAILURES\n", g_failures); return 1; }
    printf("All vial definition tests passed\n");
    return 0;
}
