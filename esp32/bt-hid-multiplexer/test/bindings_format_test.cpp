// Host-side test of main/bindings_format.cpp: the stored device bindings blob, and the conversion of
// the version 1 blob (8 entries without names) written by older firmware.
#include <stdio.h>
#include <string.h>
#include "bindings_format.h"

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)

static void put32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

int main() {
    static DeviceBindingEntry entries[MAX_DEVICE_BINDINGS];
    static DeviceBindingEntry loaded[MAX_DEVICE_BINDINGS];
    static BindingsBlob blob;
    bool needs_save = true;

    // The current version round-trips every field of every entry.
    {
        for (int i = 0; i < MAX_DEVICE_BINDINGS; i++) {
            DeviceBindingEntry &e = entries[i];
            memset(e.addr, i + 1, 6);
            e.layer = (uint8_t)(1 + i % 7);
            e.used = (i != 5);
            e.unpaired_seq = (i % 3 == 0) ? 1000u + i : 0;
            snprintf(e.name, sizeof(e.name), "Device %d", i);
        }
        memset(&blob, 0xEE, sizeof(blob));
        bindings_encode(entries, &blob);
        CHECK(blob.magic == FLASH_BINDINGS_MAGIC && blob.version == BINDINGS_VERSION && blob.reserved == 0);
        memset(loaded, 0xEE, sizeof(loaded));
        CHECK(bindings_decode(&blob, sizeof(blob), loaded, needs_save));
        CHECK(!needs_save);
        CHECK(memcmp(loaded, entries, sizeof(entries)) == 0);

        BindingsBlob bad = blob;
        bad.magic ^= 1;
        CHECK(!bindings_decode(&bad, sizeof(bad), loaded, needs_save));
        bad = blob;
        bad.version = 1;  // the size of the current version, but not its number
        CHECK(!bindings_decode(&bad, sizeof(bad), loaded, needs_save));
        bad.version = BINDINGS_VERSION + 1;
        CHECK(!bindings_decode(&bad, sizeof(bad), loaded, needs_save));
    }

    // Version 1, byte for byte as older firmware stored it: the magic, the version, 2 reserved
    // bytes, then 8 entries of 6 address bytes, the layer and the used flag.
    {
        uint8_t v1[72] = {};
        put32(v1, FLASH_BINDINGS_MAGIC);
        v1[4] = 1;
        for (int i = 0; i < 8; i++) {
            uint8_t *e = v1 + 8 + i * 8;
            memset(e, 0xA0 + i, 6);
            e[6] = (uint8_t)(i + 1 > 7 ? 7 : i + 1);
            e[7] = (i != 2);  // entry 2 is unused
        }
        memset(loaded, 0xEE, sizeof(loaded));
        CHECK(bindings_decode(v1, sizeof(v1), loaded, needs_save));
        CHECK(needs_save);
        for (int i = 0; i < 8; i++) {
            uint8_t addr[6];
            memset(addr, 0xA0 + i, 6);
            CHECK(memcmp(loaded[i].addr, addr, 6) == 0);
            CHECK(loaded[i].layer == (i + 1 > 7 ? 7 : i + 1));
            CHECK(loaded[i].used == (i != 2));
            // Left for DeviceBindings::pairingChanged() to fill in.
            CHECK(loaded[i].unpaired_seq == 0 && loaded[i].name[0] == '\0');
        }
        static const DeviceBindingEntry zero = {};
        for (int i = 8; i < MAX_DEVICE_BINDINGS; i++) {
            CHECK(memcmp(&loaded[i], &zero, sizeof(zero)) == 0);
        }

        v1[0] ^= 1;  // magic
        CHECK(!bindings_decode(v1, sizeof(v1), loaded, needs_save));
        v1[0] ^= 1;
        v1[4] = 2;   // the size of version 1, but not its number
        CHECK(!bindings_decode(v1, sizeof(v1), loaded, needs_save));
    }

    // Anything of another size is not a bindings blob.
    {
        CHECK(!bindings_decode(&blob, 0, loaded, needs_save) && !needs_save);
        CHECK(!bindings_decode(&blob, 71, loaded, needs_save));
        CHECK(!bindings_decode(&blob, sizeof(blob) - 1, loaded, needs_save));
    }

    if (g_failures) { printf("%d FAILURES\n", g_failures); return 1; }
    printf("All bindings format tests passed\n");
    return 0;
}
