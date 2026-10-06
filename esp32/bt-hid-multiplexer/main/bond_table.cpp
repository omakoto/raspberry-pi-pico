#include "bond_table.h"
#include <stdio.h>
#include <string.h>

BondTableLoad bond_table_decode(const void *data, int len, BondedTable *out) {
    memset(out, 0, sizeof(*out));
    if (len == (int)sizeof(BondedTable)) {
        BondedTable t;
        memcpy(&t, data, sizeof(t));
        if (t.count > MAX_BLE_DEVICES) return BondTableLoad::INVALID;
        *out = t;
        return BondTableLoad::CURRENT;
    }
    if (len == (int)sizeof(BondedTableV1)) {
        BondedTableV1 v1;
        memcpy(&v1, data, sizeof(v1));
        if (v1.count > MAX_BLE_DEVICES) return BondTableLoad::INVALID;
        out->count = v1.count;
        for (uint8_t i = 0; i < v1.count; i++) {
            bond_record_from_v1(&out->records[i], v1.records[i], i + 1u);
        }
        return BondTableLoad::CONVERTED;
    }
    return BondTableLoad::INVALID;
}

void bond_record_from_v1(BondedDeviceRecord *to, const BondedDeviceRecordV1 &from, uint32_t last_used) {
    memset(to, 0, sizeof(*to));
    memcpy(to->addr, from.addr, sizeof(to->addr));
    to->addr_type = from.addr_type;
    memcpy(to->name, from.name, sizeof(to->name));
    to->name[sizeof(to->name) - 1] = '\0';
    to->last_used = last_used;
}

uint32_t bond_table_max_last_used(const BondedTable &table) {
    uint32_t max = 0;
    for (uint8_t i = 0; i < table.count; i++) {
        if (table.records[i].last_used > max) max = table.records[i].last_used;
    }
    return max;
}

int bond_table_pick_eviction(const BondedTable &table, const bool connected[MAX_BLE_DEVICES]) {
    int victim = -1;
    for (uint8_t i = 0; i < table.count; i++) {
        if (victim < 0) {
            victim = i;
        } else if (connected[victim] != connected[i]) {
            if (!connected[i]) victim = i;
        } else if (table.records[i].last_used < table.records[victim].last_used) {
            victim = i;
        }
    }
    return victim;
}

BondUpsertResult bond_table_upsert(BondedTable &table, const uint8_t addr[6], uint32_t addr_type,
                                   const char *name, const char *fallback_name, bool connected,
                                   uint32_t &last_use_seq, const bool device_connected[MAX_BLE_DEVICES]) {
    BondUpsertResult result = {};
    bool has_name = name != nullptr && name[0] != '\0';
    for (uint8_t i = 0; i < table.count; i++) {
        BondedDeviceRecord &r = table.records[i];
        if (memcmp(addr, r.addr, sizeof(r.addr)) != 0) continue;
        r.addr_type = addr_type;
        if (has_name) snprintf(r.name, sizeof(r.name), "%s", name);
        if (connected) r.last_used = ++last_use_seq;
        result.index = i;
        return result;
    }

    if (table.count < MAX_BLE_DEVICES) {
        result.index = table.count++;
    } else {
        result.index = bond_table_pick_eviction(table, device_connected);
        result.evicted = true;
        result.evicted_record = table.records[result.index];
    }
    result.added = true;
    BondedDeviceRecord &r = table.records[result.index];
    memset(&r, 0, sizeof(r));
    memcpy(r.addr, addr, sizeof(r.addr));
    r.addr_type = addr_type;
    r.last_used = ++last_use_seq;
    snprintf(r.name, sizeof(r.name), "%s", has_name ? name : fallback_name);
    return result;
}
