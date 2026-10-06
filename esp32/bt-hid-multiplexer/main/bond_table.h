#ifndef BOND_TABLE_H_
#define BOND_TABLE_H_

#include <stdint.h>
#include <stdbool.h>
#include "config.h"

// The bonded device table (BleHidHost): the paired devices' addresses and names, stored in BTstack's
// TLV. This file holds its stored layout and the decisions about it that do not need BTstack, so
// that the host tests can run them (test/bond_table_test.cpp); BleHidHost does the TLV access and
// the BTstack side effects. Run test/run-host-test.sh after changing this file or bond_table.cpp.

struct BondedDeviceRecord {
    uint8_t addr[6];
    // BTstack's bd_addr_type_t, stored as the 32-bit enum it is on the ESP32 (BleHidHost checks the
    // size), so that the stored layout does not depend on BTstack's headers.
    uint32_t addr_type;
    char name[32];
    // When the device last connected, as a sequence number (higher = more recent; 0 = not since this
    // was recorded). When the table is full, the least recently used device makes room.
    uint32_t last_used;
};

struct BondedTable {
    uint8_t count;
    BondedDeviceRecord records[MAX_BLE_DEVICES];
};

// The record and table as stored before last_used existed (the table under the "HOGT" tag, and a
// single record under the older "HOGD" tag). Converted when loaded.
struct BondedDeviceRecordV1 {
    uint8_t addr[6];
    uint32_t addr_type;
    char name[32];
};

struct BondedTableV1 {
    uint8_t count;
    BondedDeviceRecordV1 records[MAX_BLE_DEVICES];
};

// The stored sizes tell the versions apart, and stored data must keep loading, so they are pinned.
static_assert(sizeof(BondedDeviceRecord) == 48, "stored layout changed");
static_assert(sizeof(BondedDeviceRecordV1) == 44, "stored layout changed");
static_assert(MAX_BLE_DEVICES != 8 || (sizeof(BondedTable) == 388 && sizeof(BondedTableV1) == 356),
              "stored layout changed");

enum class BondTableLoad {
    INVALID,    // not a table of a known version; out is empty
    CURRENT,    // stored in the current format
    CONVERTED,  // stored in the v1 format and converted; should be saved again
};

// Decodes a stored table of len bytes into out. A v1 table has no usage order, so its records get
// the table order (the order they were added in) as last_used.
BondTableLoad bond_table_decode(const void *data, int len, BondedTable *out);

// Converts a v1 record.
void bond_record_from_v1(BondedDeviceRecord *to, const BondedDeviceRecordV1 &from, uint32_t last_used);

// The highest last_used in the table (0 for an empty one).
uint32_t bond_table_max_last_used(const BondedTable &table);

// The record to drop to make room in a full table: the least recently used one, preferring a device
// that is not connected (connected[i] tells whether record i's device is). -1 for an empty table.
int bond_table_pick_eviction(const BondedTable &table, const bool connected[MAX_BLE_DEVICES]);

// What bond_table_upsert() did.
struct BondUpsertResult {
    int index;                         // the device's record
    bool added;                        // a new record (else an existing one was updated)
    bool evicted;                      // the table was full and evicted_record made room
    BondedDeviceRecord evicted_record;
};

// Adds a device to the table, or updates its record. connected says that it has just connected (or
// paired), which makes it the most recently used device; a new device always is. An empty name keeps
// the stored one, or for a new device stores fallback_name (its address). A full table makes room
// with bond_table_pick_eviction(), given which records' devices are connected; the caller then
// removes the evicted device's keys. last_use_seq is the highest last_used handed out so far.
BondUpsertResult bond_table_upsert(BondedTable &table, const uint8_t addr[6], uint32_t addr_type,
                                   const char *name, const char *fallback_name, bool connected,
                                   uint32_t &last_use_seq, const bool device_connected[MAX_BLE_DEVICES]);

#endif // BOND_TABLE_H_
