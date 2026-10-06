// Host-side test of main/bond_table.cpp: loading the stored bonded device table (including the
// conversion from the format without last_used), and which device makes room in a full table.
#include <stdio.h>
#include <string.h>
#include "bond_table.h"

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)

// Address 00:00:00:00:00:i.
static void addr_of(uint8_t i, uint8_t addr[6]) {
    memset(addr, 0, 6);
    addr[5] = i;
}

// A table of n devices "Dev i" at address i, used in the order given by last_used[].
static BondedTable make_table(uint8_t n, const uint32_t *last_used) {
    BondedTable t = {};
    t.count = n;
    for (uint8_t i = 0; i < n; i++) {
        addr_of(i, t.records[i].addr);
        t.records[i].addr_type = 1;
        snprintf(t.records[i].name, sizeof(t.records[i].name), "Dev %u", i);
        t.records[i].last_used = last_used[i];
    }
    return t;
}

static bool is_empty(const BondedTable &t) {
    static const BondedTable empty = {};
    return memcmp(&t, &empty, sizeof(t)) == 0;
}

int main() {
    const bool none_connected[MAX_BLE_DEVICES] = {};

    // A table in the current format loads as it is.
    {
        const uint32_t used[3] = {5, 9, 7};
        BondedTable stored = make_table(3, used);
        BondedTable t;
        CHECK(bond_table_decode(&stored, sizeof(stored), &t) == BondTableLoad::CURRENT);
        CHECK(memcmp(&t, &stored, sizeof(t)) == 0);
        CHECK(bond_table_max_last_used(t) == 9);

        stored.count = MAX_BLE_DEVICES + 1;  // corrupt
        CHECK(bond_table_decode(&stored, sizeof(stored), &t) == BondTableLoad::INVALID && is_empty(t));
    }

    // A table stored by the firmware before last_used existed is converted, byte for byte as that
    // firmware laid it out: a count byte, padding, then 44-byte records of the address, padding, the
    // 32-bit address type and a 32-byte name. Its order (the order the devices were added in) stands
    // in for the order of use.
    {
        uint8_t v1[sizeof(BondedTableV1)] = {};
        CHECK(sizeof(v1) == 4 + 8 * 44);
        v1[0] = 8;
        for (uint8_t i = 0; i < 8; i++) {
            uint8_t *r = v1 + 4 + i * 44;
            addr_of(i, r);
            r[8] = (uint8_t)(i % 2);  // address type, little-endian
            snprintf((char *)r + 12, 32, "Old %u", i);
        }
        memset(v1 + 4 + 7 * 44 + 12, 'x', 32);  // a name that fills its field without a terminator
        BondedTable t;
        CHECK(bond_table_decode(v1, sizeof(v1), &t) == BondTableLoad::CONVERTED);
        CHECK(t.count == 8);
        for (uint8_t i = 0; i < 8; i++) {
            uint8_t addr[6];
            addr_of(i, addr);
            CHECK(memcmp(t.records[i].addr, addr, 6) == 0);
            CHECK(t.records[i].addr_type == i % 2u);
            CHECK(t.records[i].last_used == i + 1u);
        }
        CHECK(strcmp(t.records[0].name, "Old 0") == 0 && strcmp(t.records[6].name, "Old 6") == 0);
        CHECK(strlen(t.records[7].name) == 31);
        CHECK(bond_table_max_last_used(t) == 8);

        v1[0] = MAX_BLE_DEVICES + 1;  // corrupt
        CHECK(bond_table_decode(v1, sizeof(v1), &t) == BondTableLoad::INVALID && is_empty(t));
    }

    // Anything of another size is not a table (nothing stored reads as 0 bytes).
    {
        uint8_t junk[sizeof(BondedTable) + 1] = {};
        BondedTable t;
        CHECK(bond_table_decode(junk, 0, &t) == BondTableLoad::INVALID && is_empty(t));
        CHECK(bond_table_decode(junk, sizeof(BondedTableV1) - 1, &t) == BondTableLoad::INVALID);
        CHECK(bond_table_decode(junk, sizeof(BondedTable) + 1, &t) == BondTableLoad::INVALID);
        CHECK(bond_table_max_last_used(t) == 0);
    }

    // The single record of the oldest firmware converts the same way.
    {
        BondedDeviceRecordV1 legacy = {};
        addr_of(3, legacy.addr);
        legacy.addr_type = 1;
        strcpy(legacy.name, "Legacy");
        BondedDeviceRecord r;
        memset(&r, 0xFF, sizeof(r));
        bond_record_from_v1(&r, legacy, 1);
        CHECK(r.addr[5] == 3 && r.addr_type == 1 && strcmp(r.name, "Legacy") == 0 && r.last_used == 1);
    }

    // The least recently used device makes room, preferring one that is not connected.
    {
        const uint32_t used[8] = {40, 20, 70, 10, 80, 30, 60, 50};
        BondedTable t = make_table(8, used);
        CHECK(bond_table_pick_eviction(t, none_connected) == 3);
        bool connected[MAX_BLE_DEVICES] = {};
        connected[3] = true;
        CHECK(bond_table_pick_eviction(t, connected) == 1);
        connected[1] = connected[5] = connected[0] = true;
        CHECK(bond_table_pick_eviction(t, connected) == 7);  // 50: the oldest of those not connected
        for (bool &c : connected) c = true;
        CHECK(bond_table_pick_eviction(t, connected) == 3);  // all connected: the oldest anyway
        connected[6] = false;
        CHECK(bond_table_pick_eviction(t, connected) == 6);  // the only one not connected
        // Records found without metadata (last_used 0) go first; ties go to the first record.
        t.records[4].last_used = 0;
        t.records[2].last_used = 0;
        CHECK(bond_table_pick_eviction(t, none_connected) == 2);
        BondedTable empty = {};
        CHECK(bond_table_pick_eviction(empty, none_connected) == -1);
    }

    // Adding and updating devices.
    {
        BondedTable t = {};
        uint32_t seq = 0;
        uint8_t addr[6];

        // New devices are appended, most recently used; without a name they get the fallback.
        addr_of(1, addr);
        BondUpsertResult r = bond_table_upsert(t, addr, 0, "One", "fallback", false, seq, none_connected);
        CHECK(r.index == 0 && r.added && !r.evicted && t.count == 1 && seq == 1);
        CHECK(t.records[0].last_used == 1 && strcmp(t.records[0].name, "One") == 0);
        addr_of(2, addr);
        r = bond_table_upsert(t, addr, 1, "", "00:00:00:00:00:02", true, seq, none_connected);
        CHECK(r.index == 1 && r.added && t.count == 2 && t.records[1].last_used == 2);
        CHECK(strcmp(t.records[1].name, "00:00:00:00:00:02") == 0 && t.records[1].addr_type == 1);

        // An update: a name read (not a connection) keeps the order of use; an empty name keeps the
        // stored one; a connection makes the device the most recently used.
        addr_of(1, addr);
        r = bond_table_upsert(t, addr, 1, "Renamed", "fallback", false, seq, none_connected);
        CHECK(r.index == 0 && !r.added && !r.evicted && t.count == 2 && seq == 2);
        CHECK(t.records[0].last_used == 1 && strcmp(t.records[0].name, "Renamed") == 0);
        CHECK(t.records[0].addr_type == 1);
        r = bond_table_upsert(t, addr, 1, nullptr, "fallback", true, seq, none_connected);
        CHECK(r.index == 0 && !r.added && seq == 3 && t.records[0].last_used == 3);
        CHECK(strcmp(t.records[0].name, "Renamed") == 0);

        // A full table: the least recently used device not connected makes room, and the caller
        // gets its record to remove its keys.
        for (uint8_t i = 3; i <= MAX_BLE_DEVICES; i++) {
            addr_of(i, addr);
            bond_table_upsert(t, addr, 0, "More", "fallback", true, seq, none_connected);
        }
        CHECK(t.count == MAX_BLE_DEVICES);
        // Records 0..7 were last used at 3, 2, 4..9; record 1 (address 2) is the oldest.
        bool connected[MAX_BLE_DEVICES] = {};
        connected[1] = true;
        addr_of(9, addr);
        r = bond_table_upsert(t, addr, 0, "Ninth", "fallback", true, seq, connected);
        CHECK(r.added && r.evicted && r.index == 0);  // address 1, since address 2 is connected
        CHECK(r.evicted_record.addr[5] == 1 && strcmp(r.evicted_record.name, "Renamed") == 0);
        CHECK(t.count == MAX_BLE_DEVICES && t.records[0].addr[5] == 9);
        CHECK(strcmp(t.records[0].name, "Ninth") == 0 && t.records[0].last_used == seq);
        CHECK(bond_table_max_last_used(t) == seq);
        // The device just added is the most recently used, so it is not the next to go.
        addr_of(10, addr);
        r = bond_table_upsert(t, addr, 0, "Tenth", "fallback", true, seq, none_connected);
        CHECK(r.evicted && r.evicted_record.addr[5] == 2 && r.index == 1);
    }

    if (g_failures) { printf("%d FAILURES\n", g_failures); return 1; }
    printf("All bond table tests passed\n");
    return 0;
}
