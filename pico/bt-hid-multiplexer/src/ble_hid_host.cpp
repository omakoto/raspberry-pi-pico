#include "ble_hid_host.h"
#include "log_ring.h"
#include "pico/time.h"
#include "classic_hid_host.h"
#include "multiplexer.h"
#include "config.h"
#include "btstack.h"
#include "btstack_tlv.h"
#include "ble/gatt-service/hids_client.h"
#include "ble/le_device_db.h"
#include "hci_dump.h"
#include "pico/time.h"
#include <stdio.h>
#include <string.h>

// Storage tags for persisting bonded device metadata in flash TLV
#define TLV_TAG_HOGD       ((((uint32_t) 'H') << 24 ) | (((uint32_t) 'O') << 16) | (((uint32_t) 'G') << 8) | 'D')
#define TLV_TAG_HOG_TABLE  ((((uint32_t) 'H') << 24 ) | (((uint32_t) 'O') << 16) | (((uint32_t) 'G') << 8) | 'T')

struct BondedDeviceRecord {
    bd_addr_t addr;
    bd_addr_type_t addr_type;
    char name[32];
};

struct BondedTable {
    uint8_t count;
    BondedDeviceRecord records[MAX_BLE_DEVICES];
};

enum BleMouseFormat : uint8_t {
    MOUSE_FORMAT_UNKNOWN = 0,
    MOUSE_FORMAT_STANDARD,       // 8-bit relative dx, dy
    MOUSE_FORMAT_16BIT,          // 16-bit relative dx, dy (ProtoArc trackpad)
    MOUSE_FORMAT_LOGITECH_12BIT  // 16-bit buttons, 12-bit packed dx, dy (Logitech Lift / MX)
};

struct BleSlot {
    hci_con_handle_t con_handle;
    uint16_t hids_cid;
    bd_addr_t addr;
    bd_addr_type_t addr_type;
    char name[32];
    uint8_t dev_idx;
    bool connected;
    uint16_t conn_interval;     // last reported LL connection interval, 1.25 ms units (0 = unknown)
    uint16_t min_conn_interval; // shortest LL connection interval observed on this connection (1.25 ms units)
    uint16_t conn_latency;      // last reported slave latency
    uint8_t latency_forced;     // how often parameter optimization was requested on this connection
    btstack_timer_source_t latency_timer; // delays parameter optimization past the peer's settling window
    bool has_led_report;        // peripheral provides an LED output report
    bool led_report_resolved;   // report descriptor has been inspected for LED output report
    uint8_t led_report_id;      // report ID for LED output report (0 if no report ID)
    uint32_t last_used_ms;      // timestamp (ms since boot) of last input report or connection
    BleMouseFormat mouse_format;     // detected mouse report format
    bool mouse_format_resolved;      // report descriptor has been inspected for mouse format
    uint8_t mouse_report_id;         // report ID for mouse input reports
    uint16_t mouse_speed_percent;    // mouse sensitivity scaling (default 100)
    int32_t scale_rem_x;             // fractional count accumulator for X
    int32_t scale_rem_y;             // fractional count accumulator for Y
};

// Peripherals defend their preferred parameters for a while after connecting: the ProtoArc XK01
// immediately re-requests latency 32 whenever the parameters change in the first seconds, but
// accepts latency 0 once the link has settled. So the request is made this long after the
// peripheral's last parameter update, and at most a few times per connection before giving up.
#define ZERO_LATENCY_DELAY_MS     10000
#define MAX_ZERO_LATENCY_ATTEMPTS 3

static BleSlot s_slots[MAX_BLE_DEVICES];
static BondedTable s_bonded_table;
static uint16_t s_global_mouse_speed_percent = 100;

static bool s_is_scanning = false;
static bool s_is_connecting = false;
static int8_t s_connecting_slot_idx = -1;
static uint32_t s_active_passkey = 0;
static hci_con_handle_t s_pending_pairing_handle = HCI_CON_HANDLE_INVALID;
static hid_protocol_mode_t s_protocol_mode = HID_PROTOCOL_MODE_REPORT;
static char s_dev_name_summary[64] = {0};

// Pairing mode state: when active, unbonded target HID devices are accepted for connection.
// When inactive, background scanning strictly reconnects to already-bonded devices.
static bool s_is_pairing_mode = false;
static uint32_t s_pairing_mode_start_ms = 0;
static uint32_t s_pairing_mode_duration_ms = 0;
static btstack_timer_source_t s_pairing_mode_timer;

// Pending connection queued while disconnecting the least-recently-used slot
static bool s_eviction_pending = false;
static bd_addr_t s_pending_connect_addr;
static bd_addr_type_t s_pending_connect_addr_type;
static char s_pending_connect_name[32];

// Authentication requirements we put in our SMP Pairing Request. Only affects new pairings;
// existing bonds re-encrypt with their stored LTK regardless.
//
// Default is LE legacy pairing without MITM. The ProtoArc XK01 keyboard only ever delivered
// input reports when paired this way: once LE Secure Connections pairing was offered it paired
// and encrypted fine but never sent a single HID notification. Other devices (e.g. the Keychron
// Nape Pro on one of its host slots) accept only Secure Connections and drop the link when
// offered legacy pairing. So each failed pairing attempt during a pairing window flips the
// Secure Connections bit for the next attempt (s_sc_flipped): every device gets the default
// first and the other method next. 'authreq' sets the default.
// MITM (passkey entry, shown on the OLED) is opt-in because it changes the pairing UX.
static uint8_t s_sm_auth_req = SM_AUTHREQ_BONDING;
static bool s_sc_flipped = false;

// The authentication requirements offered right now: the configured policy, with the Secure
// Connections bit flipped after a failed attempt in the current pairing window.
static uint8_t effective_auth_req() {
    return s_sm_auth_req ^ (s_sc_flipped ? SM_AUTHREQ_SECURE_CONNECTION : 0);
}

static void apply_auth_req(bool sc_flipped) {
    s_sc_flipped = sc_flipped;
    sm_set_authentication_requirements(effective_auth_req());
}
static bool s_stack_logging = false;
static bool s_log_reports = false;

// Connection and discovery timers
static btstack_timer_source_t s_reconnect_timer;
static btstack_timer_source_t s_pairing_timer;

static void onReconnectTimeout(btstack_timer_source_t *ts);
static void onPairingDelayTimeout(btstack_timer_source_t *ts);
static void onPairingModeTimeout(btstack_timer_source_t *ts);
static void tryAutoReconnectOrScan();
static void connectToDevice(const bd_addr_t addr, bd_addr_type_t addr_type, const char *name);
static void evict_lru_and_connect(const bd_addr_t addr, bd_addr_type_t addr_type, const char *name);

// Storage buffer for HID Report Descriptors across all active HOGP instances
static uint8_t s_hid_descriptor_storage[4096];
static btstack_packet_callback_registration_t s_hci_event_callback_registration;
static btstack_packet_callback_registration_t s_sm_event_callback_registration;

// Simple GATT Database for GAP service
static const uint8_t s_profile_data[] = {
    1, // Version
    0x0a, 0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0x28, 0x00, 0x18,
    0x0d, 0x00, 0x02, 0x00, 0x02, 0x00, 0x03, 0x28, 0x02, 0x03, 0x00, 0x00, 0x2a,
    0x10, 0x00, 0x02, 0x00, 0x03, 0x00, 0x00, 0x2a, 'H', 'O', 'G', ' ', 'H', 'o', 's', 't',
    0x00, 0x00
};

// Slot lookup helpers
static BleSlot* find_slot_by_handle(hci_con_handle_t handle) {
    if (handle == HCI_CON_HANDLE_INVALID) return nullptr;
    for (uint8_t i = 0; i < MAX_BLE_DEVICES; i++) {
        if (s_slots[i].con_handle == handle) return &s_slots[i];
    }
    return nullptr;
}

static BleSlot* find_slot_by_cid(uint16_t cid) {
    if (cid == 0) return nullptr;
    for (uint8_t i = 0; i < MAX_BLE_DEVICES; i++) {
        if (s_slots[i].hids_cid == cid) return &s_slots[i];
    }
    return nullptr;
}

static BleSlot* find_slot_by_addr(const bd_addr_t addr) {
    for (uint8_t i = 0; i < MAX_BLE_DEVICES; i++) {
        if ((s_slots[i].connected || s_slots[i].con_handle != HCI_CON_HANDLE_INVALID) &&
            bd_addr_cmp(addr, s_slots[i].addr) == 0) {
            return &s_slots[i];
        }
    }
    return nullptr;
}

static BleSlot* find_free_slot() {
    for (uint8_t i = 0; i < MAX_BLE_DEVICES; i++) {
        if (!s_slots[i].connected && s_slots[i].con_handle == HCI_CON_HANDLE_INVALID && s_connecting_slot_idx != (int8_t)i) {
            return &s_slots[i];
        }
    }
    return nullptr;
}

struct AdvDeviceInfo {
    bool has_hid_service;
    uint16_t appearance;
    char name[32];
};

static bool name_matches_hid_keywords(const char *name) {
    if (!name || name[0] == '\0') return false;
    char lower[32];
    size_t i = 0;
    for (; name[i] && i < sizeof(lower) - 1; ++i) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') c += ('a' - 'A');
        lower[i] = c;
    }
    lower[i] = '\0';

    return (strstr(lower, "protoarc") != NULL ||
            strstr(lower, "keyboard") != NULL ||
            strstr(lower, "kbd")      != NULL ||
            strstr(lower, "keychron") != NULL ||
            strstr(lower, "trackpad") != NULL ||
            strstr(lower, "touchpad") != NULL ||
            strstr(lower, "trackball")!= NULL ||
            strstr(lower, "nape")     != NULL ||
            strstr(lower, "mouse")    != NULL ||
            strstr(lower, "xk01")     != NULL ||
            strstr(lower, "k380")     != NULL ||
            strstr(lower, "k480")     != NULL ||
            strstr(lower, "logi")     != NULL);
}

static void parse_adv_data(const uint8_t *ad_data, uint8_t ad_len, AdvDeviceInfo *info) {
    info->has_hid_service = false;
    info->appearance = 0;
    info->name[0] = '\0';

    if (!ad_data || ad_len == 0) return;

    if (ad_data_contains_uuid16(ad_len, ad_data, ORG_BLUETOOTH_SERVICE_HUMAN_INTERFACE_DEVICE)) {
        info->has_hid_service = true;
    }

    ad_context_t context;
    ad_iterator_init(&context, ad_len, ad_data);
    while (ad_iterator_has_more(&context)) {
        uint8_t data_type = ad_iterator_get_data_type(&context);
        uint8_t data_len = ad_iterator_get_data_len(&context);
        const uint8_t *data = ad_iterator_get_data(&context);

        switch (data_type) {
            case BLUETOOTH_DATA_TYPE_SHORTENED_LOCAL_NAME:
            case BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME: {
                if (info->name[0] == '\0' && data_len > 0) {
                    uint8_t copy_len = (data_len < sizeof(info->name) - 1) ? data_len : (sizeof(info->name) - 1);
                    memcpy(info->name, data, copy_len);
                    info->name[copy_len] = '\0';
                }
                break;
            }

            case BLUETOOTH_DATA_TYPE_APPEARANCE: {
                if (data_len >= 2) {
                    info->appearance = (uint16_t)(data[0] | (data[1] << 8));
                }
                break;
            }

            default:
                break;
        }
        ad_iterator_next(&context);
    }
}

static bool is_target_hid_device(const AdvDeviceInfo &info) {
    if (info.has_hid_service) return true;
    if (info.appearance >= 0x03C0 && info.appearance <= 0x03CF) return true;
    if (name_matches_hid_keywords(info.name)) return true;
    return false;
}

struct SeenDevice {
    bd_addr_t addr;
    uint32_t last_seen_ms;
    bool has_hid_service;
    uint16_t appearance;
    char name[32];
};
static SeenDevice s_seen_devices[64];
static uint8_t s_seen_idx = 0;

static bool update_and_should_log_device(const bd_addr_t addr, uint32_t now, AdvDeviceInfo *info) {
    for (uint8_t i = 0; i < 64; ++i) {
        if (bd_addr_cmp(addr, s_seen_devices[i].addr) == 0) {
            if (info->name[0] != '\0') {
                memcpy(s_seen_devices[i].name, info->name, sizeof(s_seen_devices[i].name));
            } else if (s_seen_devices[i].name[0] != '\0') {
                memcpy(info->name, s_seen_devices[i].name, sizeof(info->name));
            }

            if (info->has_hid_service) {
                s_seen_devices[i].has_hid_service = true;
            } else if (s_seen_devices[i].has_hid_service) {
                info->has_hid_service = true;
            }

            if (info->appearance != 0) {
                s_seen_devices[i].appearance = info->appearance;
            } else if (s_seen_devices[i].appearance != 0) {
                info->appearance = s_seen_devices[i].appearance;
            }

            if (now - s_seen_devices[i].last_seen_ms < 4000) {
                return false;
            }
            s_seen_devices[i].last_seen_ms = now;
            return true;
        }
    }

    bd_addr_copy(s_seen_devices[s_seen_idx].addr, addr);
    s_seen_devices[s_seen_idx].last_seen_ms = now;
    s_seen_devices[s_seen_idx].has_hid_service = info->has_hid_service;
    s_seen_devices[s_seen_idx].appearance = info->appearance;
    memcpy(s_seen_devices[s_seen_idx].name, info->name, sizeof(s_seen_devices[s_seen_idx].name));
    s_seen_idx = (s_seen_idx + 1) % 64;
    return true;
}

// Persist the full table of bonded devices to TLV storage
static void save_bonded_devices() {
    const btstack_tlv_t *tlv_impl = nullptr;
    void *tlv_context = nullptr;
    btstack_tlv_get_instance(&tlv_impl, &tlv_context);
    if (!tlv_impl) return;

    tlv_impl->store_tag(tlv_context, TLV_TAG_HOG_TABLE, (const uint8_t *)&s_bonded_table, sizeof(s_bonded_table));
    printf("[BLE Host] Persisted %u bonded device(s) to TLV table.\n", s_bonded_table.count);
}

// Load bonded devices table from TLV, migrating single-device legacy entries if present
static void load_bonded_devices() {
    memset(&s_bonded_table, 0, sizeof(s_bonded_table));

    const btstack_tlv_t *tlv_impl = nullptr;
    void *tlv_context = nullptr;
    btstack_tlv_get_instance(&tlv_impl, &tlv_context);
    if (!tlv_impl) return;

    int len = tlv_impl->get_tag(tlv_context, TLV_TAG_HOG_TABLE, (uint8_t *)&s_bonded_table, sizeof(s_bonded_table));
    if (len == (int)sizeof(s_bonded_table) && s_bonded_table.count <= MAX_BLE_DEVICES) {
        printf("[BLE Host] Loaded %u bonded device(s) from TLV table.\n", s_bonded_table.count);
    } else {
        // Fall back to legacy single-device TLV tag if present
        BondedDeviceRecord legacy_record;
        len = tlv_impl->get_tag(tlv_context, TLV_TAG_HOGD, (uint8_t *)&legacy_record, sizeof(legacy_record));
        if (len == (int)sizeof(legacy_record)) {
            s_bonded_table.count = 1;
            memcpy(&s_bonded_table.records[0], &legacy_record, sizeof(legacy_record));
            save_bonded_devices();
            printf("[BLE Host] Migrated legacy bonded device '%s' (%s) to table.\n",
                   legacy_record.name, bd_addr_to_str(legacy_record.addr));
        }
    }

    // Inspect le_device_db to discover any keys present without a metadata record
    for (int i = 0; i < le_device_db_max_count(); i++) {
        int addr_type = 0;
        bd_addr_t db_addr;
        le_device_db_info(i, &addr_type, db_addr, nullptr);
        if (addr_type != BD_ADDR_TYPE_UNKNOWN) {
            bool exists = false;
            for (uint8_t r = 0; r < s_bonded_table.count; r++) {
                if (bd_addr_cmp(db_addr, s_bonded_table.records[r].addr) == 0) {
                    exists = true;
                    break;
                }
            }
            if (!exists && s_bonded_table.count < MAX_BLE_DEVICES) {
                uint8_t idx = s_bonded_table.count++;
                bd_addr_copy(s_bonded_table.records[idx].addr, db_addr);
                s_bonded_table.records[idx].addr_type = (bd_addr_type_t)addr_type;
                snprintf(s_bonded_table.records[idx].name, sizeof(s_bonded_table.records[idx].name),
                         "%s", bd_addr_to_str(db_addr));
                save_bonded_devices();
                printf("[BLE Host] Added database peripheral '%s' to bonded table.\n",
                       s_bonded_table.records[idx].name);
            }
        }
    }
}

// Add or update an entry in the bonded devices table
static void add_or_update_bonded_device(const bd_addr_t addr, bd_addr_type_t addr_type, const char *name) {
    for (uint8_t i = 0; i < s_bonded_table.count; i++) {
        if (bd_addr_cmp(addr, s_bonded_table.records[i].addr) == 0) {
            s_bonded_table.records[i].addr_type = addr_type;
            if (name && name[0] != '\0') {
                snprintf(s_bonded_table.records[i].name, sizeof(s_bonded_table.records[i].name), "%s", name);
            }
            save_bonded_devices();
            return;
        }
    }

    uint8_t target_idx = 0;
    if (s_bonded_table.count < MAX_BLE_DEVICES) {
        target_idx = s_bonded_table.count++;
    } else {
        target_idx = 0; // Overwrite oldest slot if capacity reached
    }

    bd_addr_copy(s_bonded_table.records[target_idx].addr, addr);
    s_bonded_table.records[target_idx].addr_type = addr_type;
    if (name && name[0] != '\0') {
        snprintf(s_bonded_table.records[target_idx].name, sizeof(s_bonded_table.records[target_idx].name), "%s", name);
    } else {
        snprintf(s_bonded_table.records[target_idx].name, sizeof(s_bonded_table.records[target_idx].name),
                 "%s", bd_addr_to_str(addr));
    }

    save_bonded_devices();
    printf("[BLE Host] Saved bonded device [%u]: '%s' (%s)\n",
           target_idx, s_bonded_table.records[target_idx].name, bd_addr_to_str(addr));
}

static bool is_bonded_device_addr(const bd_addr_t addr) {
    for (uint8_t i = 0; i < s_bonded_table.count; i++) {
        if (bd_addr_cmp(addr, s_bonded_table.records[i].addr) == 0) {
            return true;
        }
    }
    for (int i = 0; i < le_device_db_max_count(); i++) {
        int addr_type = 0;
        bd_addr_t db_addr;
        le_device_db_info(i, &addr_type, db_addr, nullptr);
        if (addr_type != BD_ADDR_TYPE_UNKNOWN && bd_addr_cmp(addr, db_addr) == 0) {
            return true;
        }
    }
    return false;
}

// Timeout handler for targeted auto-reconnect or connection attempts
static void onReconnectTimeout(btstack_timer_source_t *ts) {
    (void)ts;
    if (s_is_connecting) {
        printf("[BLE Host] Connection attempt timed out. Cancelling...\n");
        s_eviction_pending = false;
        // Instruct BTstack controller to cancel the pending LE connection.
        // The controller will emit GAP_SUBEVENT_LE_CONNECTION_COMPLETE with an error status,
        // which cleanly clears the connecting slot and safely resumes scanning without HCI race conditions.
        gap_connect_cancel();
    }
}

static void onPairingModeTimeout(btstack_timer_source_t *ts) {
    (void)ts;
    printf("[BLE Host] Pairing mode timed out (60s). Returning to bonded-only scan.\n");
    BleHidHost::stopPairingMode();
}

// Delay timer to let link layer connection anchors stabilize before initiating SMP security
static void onPairingDelayTimeout(btstack_timer_source_t *ts) {
    (void)ts;
    if (s_pending_pairing_handle != HCI_CON_HANDLE_INVALID) {
        printf("[BLE Host] Requesting security/pairing for handle 0x%04X...\n", s_pending_pairing_handle);
        sm_request_pairing(s_pending_pairing_handle);
        s_pending_pairing_handle = HCI_CON_HANDLE_INVALID;
    }
}

#if BLE_ZERO_SLAVE_LATENCY
static void onZeroLatencyTimeout(btstack_timer_source_t *ts) {
    BleSlot *slot = (BleSlot *)btstack_run_loop_get_timer_context(ts);
    if (!slot || slot->con_handle == HCI_CON_HANDLE_INVALID) return;
    uint16_t target_interval = (slot->min_conn_interval > 0) ? slot->min_conn_interval : slot->conn_interval;
    if (slot->conn_latency == 0 && (target_interval == 0 || slot->conn_interval <= target_interval)) return;
    if (slot->latency_forced >= MAX_ZERO_LATENCY_ATTEMPTS) {
        printf("[BLE Host] Slot %u keeps renegotiating non-optimal params (interval %.2f ms, latency %u); leaving it alone.\n",
               slot->dev_idx, slot->conn_interval * 1.25f, slot->conn_latency);
        return;
    }
    slot->latency_forced++;
    BleHidHost::updateConnectionParams(slot->dev_idx, target_interval, 0);
}

static void scheduleZeroLatency(BleSlot *slot) {
    btstack_run_loop_remove_timer(&slot->latency_timer);
    btstack_run_loop_set_timer_context(&slot->latency_timer, slot);
    btstack_run_loop_set_timer_handler(&slot->latency_timer, &onZeroLatencyTimeout);
    btstack_run_loop_set_timer(&slot->latency_timer, ZERO_LATENCY_DELAY_MS);
    btstack_run_loop_add_timer(&slot->latency_timer);
}
#endif

// Forget everything about a slot. The latency timer must leave the run loop before the struct
// is zeroed, or the run loop keeps a dangling list entry.
static void reset_slot(BleSlot *slot) {
    btstack_run_loop_remove_timer(&slot->latency_timer);
    uint8_t idx = (uint8_t)(slot - s_slots);
    memset(slot, 0, sizeof(BleSlot));
    slot->dev_idx = idx;
    slot->con_handle = HCI_CON_HANDLE_INVALID;
    slot->mouse_format = MOUSE_FORMAT_UNKNOWN;
    slot->mouse_speed_percent = s_global_mouse_speed_percent;
}

// Disconnect the least-recently-used connected slot to make room for a new connection
static void evict_lru_and_connect(const bd_addr_t addr, bd_addr_type_t addr_type, const char *name) {
    if (s_eviction_pending || s_is_connecting) return;

    int8_t lru_idx = -1;
    uint32_t oldest_ms = 0xFFFFFFFF;
    for (uint8_t i = 0; i < MAX_BLE_DEVICES; i++) {
        if (s_connecting_slot_idx == (int8_t)i) continue;
        if (s_slots[i].connected || s_slots[i].con_handle != HCI_CON_HANDLE_INVALID) {
            if (s_slots[i].last_used_ms < oldest_ms) {
                oldest_ms = s_slots[i].last_used_ms;
                lru_idx = i;
            }
        }
    }
    if (lru_idx < 0) {
        printf("[BLE Host] Max connection limit reached (%d devices). No connected slot eligible for eviction.\n", MAX_BLE_DEVICES);
        return;
    }

    BleSlot *evicted = &s_slots[lru_idx];
    uint32_t now = to_ms_since_boot(get_absolute_time());
    uint32_t idle_s = (now >= evicted->last_used_ms) ? (now - evicted->last_used_ms) / 1000 : 0;
    printf("[BLE Host] Connection capacity (%d) reached. Evicting LRU slot %u ('%s', idle %lu s) to connect '%s'...\n",
           MAX_BLE_DEVICES, lru_idx, evicted->name, (unsigned long)idle_s,
           (name && name[0] != '\0') ? name : bd_addr_to_str(addr));

    s_eviction_pending = true;
    bd_addr_copy(s_pending_connect_addr, addr);
    s_pending_connect_addr_type = addr_type;
    if (name && name[0] != '\0') {
        snprintf(s_pending_connect_name, sizeof(s_pending_connect_name), "%s", name);
    } else {
        snprintf(s_pending_connect_name, sizeof(s_pending_connect_name), "%s", bd_addr_to_str(addr));
    }

    gap_disconnect(evicted->con_handle);
}

// Centralized connection routine with 10-second timeout
static void connectToDevice(const bd_addr_t addr, bd_addr_type_t addr_type, const char *name) {
    if (s_is_connecting) return;
    if (find_slot_by_addr(addr) != nullptr) return;

    BleSlot *slot = find_free_slot();
    if (!slot) {
        evict_lru_and_connect(addr, addr_type, name);
        return;
    }

    slot->con_handle = HCI_CON_HANDLE_INVALID;
    slot->hids_cid = 0;
    slot->connected = false;
    slot->last_used_ms = to_ms_since_boot(get_absolute_time());
    bd_addr_copy(slot->addr, addr);
    slot->addr_type = addr_type;
    if (name && name[0] != '\0') {
        snprintf(slot->name, sizeof(slot->name), "%s", name);
    } else {
        snprintf(slot->name, sizeof(slot->name), "%s", bd_addr_to_str(addr));
    }

    s_connecting_slot_idx = slot->dev_idx;
    s_is_connecting = true;

    // Temporarily halt scanning while controller initiates connection
    gap_stop_scan();
    s_is_scanning = false;

    printf("[BLE Host] Connecting to '%s' (%s, type %u) in slot %u...\n",
           slot->name, bd_addr_to_str(slot->addr), (unsigned)slot->addr_type, slot->dev_idx);

    btstack_run_loop_set_timer(&s_reconnect_timer, 10000);
    btstack_run_loop_set_timer_handler(&s_reconnect_timer, &onReconnectTimeout);
    btstack_run_loop_add_timer(&s_reconnect_timer);

    gap_connect(slot->addr, slot->addr_type);
}

static void tryAutoReconnectOrScan() {
    uint8_t conn_count = BleHidHost::getConnectedCount();
    printf("[BLE Host] %u / %u devices connected, %u bonded device(s) stored.\n",
           conn_count, MAX_BLE_DEVICES, s_bonded_table.count);
    if (BleHidHost::hasUnconnectedBonds() || BleHidHost::isPairingMode()) {
        BleHidHost::startScan();
    }
}

// ---- Filtered BTstack log sink ---------------------------------------------------------------
// BTstack's log_info output is emitted from the BTstack run-loop context, which runs at a higher
// priority than main() on this port, and every line goes out over the blocking UART console.
// Unfiltered, hci.c and l2cap.c alone print hundreds of lines at boot; that starved the main loop
// (and TinyUSB's tud_task) long enough for USB enumeration to fail. Only the modules that explain
// pairing and HID-over-GATT problems are let through, and the long absolute source path BTstack
// prefixes each line with is trimmed to the file name. Errors from any module always pass.
static const char *const s_stack_log_modules[] = { "sm.c", "gatt_client.c", "hids_client.c" };

static void stack_log_reset(void) {}

// Raw HCI packet dump, off by default and meant for short, targeted captures with scanning stopped
// (every advertising report is an HCI event and the console would drown). Only ACL data is printed,
// which is where ATT requests/responses and notifications travel.
static void stack_log_packet(uint8_t packet_type, uint8_t in, uint8_t *packet, uint16_t len) {
    if (packet_type != HCI_ACL_DATA_PACKET) return;
    printf("[HCI] ACL %s (%u bytes):", in ? "<=" : "=>", len);
    for (uint16_t i = 0; i < len && i < 40; i++) {
        printf(" %02X", packet[i]);
    }
    printf("%s\n", len > 40 ? " ..." : "");
}

// ---------------------------------------------------------------------------------------------
// Heartbeat and self-limiting diagnostics
// ---------------------------------------------------------------------------------------------

// A BTstack timer that ticks once a second proves the Bluetooth run loop is alive; the main loop
// only feeds the watchdog while it does (see isAlive()).
static btstack_timer_source_t s_heartbeat_timer;
static volatile uint32_t s_heartbeat_ms = 0;
static bool s_heartbeat_started = false;

// The verbose console dumps (reports, hcilog, log) are switched off again after this long, so that
// a forgotten one cannot flood the console and the log ring.
static const uint32_t VERBOSE_AUTO_OFF_MS = 15000;
static uint32_t s_verbose_off_at_ms = 0;
static bool s_logging_ready = false;  // false while init() applies the defaults
static bool s_hci_logging = false;

static void arm_verbose_auto_off() {
    if (!s_logging_ready) return;
    s_verbose_off_at_ms = to_ms_since_boot(get_absolute_time()) + VERBOSE_AUTO_OFF_MS;
    if (s_verbose_off_at_ms == 0) s_verbose_off_at_ms = 1;
}

static void onHeartbeat(btstack_timer_source_t *ts) {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    s_heartbeat_ms = now;
    if (s_verbose_off_at_ms != 0 && now >= s_verbose_off_at_ms) {
        s_verbose_off_at_ms = 0;
        printf("[BLE Host] Verbose dumps switched off automatically after %lu s.\n",
               (unsigned long)(VERBOSE_AUTO_OFF_MS / 1000));
        if (s_log_reports) BleHidHost::setReportLogging(false);
        if (s_hci_logging) BleHidHost::setHciPacketLogging(false);
        if (s_stack_logging) BleHidHost::setStackLogging(false);
    }
    btstack_run_loop_set_timer(ts, 1000);
    btstack_run_loop_add_timer(ts);
}

bool BleHidHost::isAlive(uint32_t now_ms) {
    return !s_heartbeat_started || (now_ms - s_heartbeat_ms) < 3000;
}

// Limits the raw report dump to a handful of lines per second. Every line costs time in the
// Bluetooth context, and a busy mouse sends ~130 reports/s.
static bool report_log_allowed() {
    static uint32_t window_start_ms = 0;
    static uint32_t lines = 0, suppressed = 0;
    const uint32_t MAX_LINES_PER_SECOND = 40;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - window_start_ms >= 1000) {
        if (suppressed > 0) {
            printf("[BLE Host] (%lu report lines suppressed in the last second)\n", (unsigned long)suppressed);
        }
        window_start_ms = now;
        lines = 0;
        suppressed = 0;
    }
    if (lines >= MAX_LINES_PER_SECOND) {
        suppressed++;
        return false;
    }
    lines++;
    return true;
}

void BleHidHost::setReportLogging(bool enable) {
    s_log_reports = enable;
    if (enable) arm_verbose_auto_off();
    printf("[BLE Host] Raw HID report dump %s.\n", enable ? "enabled" : "disabled");
}

void BleHidHost::setHciPacketLogging(bool enable) {
    s_hci_logging = enable;
    if (enable) arm_verbose_auto_off();
    hci_dump_enable_packet_log(enable);
    printf("[BLE Host] HCI ACL packet dump %s.\n", enable ? "enabled" : "disabled");
}

static void stack_log_message(int log_level, const char *format, va_list args) {
    static char buf[200];
    int len = vsnprintf(buf, sizeof(buf), format, args);
    if (len <= 0) return;

    // Lines look like "<path>/sm.c.1260: text"; locate the file name just after the last '/'
    // that precedes the first ": " separator.
    const char *text = buf;
    const char *sep = strstr(buf, ": ");
    if (sep) {
        for (const char *p = sep; p > buf; p--) {
            if (p[-1] == '/') { text = p; break; }
        }
    }

    bool pass = (log_level == HCI_DUMP_LOG_LEVEL_ERROR);
    for (size_t i = 0; !pass && i < sizeof(s_stack_log_modules) / sizeof(s_stack_log_modules[0]); i++) {
        size_t n = strlen(s_stack_log_modules[i]);
        pass = (strncmp(text, s_stack_log_modules[i], n) == 0 && text[n] == '.');
    }
    if (pass) {
        printf("[BT] %s\n", text);
    }
}

static const hci_dump_t s_stack_log_sink = {
    &stack_log_reset,
    &stack_log_packet,
    &stack_log_message,
};

// Prints the security properties of an encrypted link so pairing problems are visible on the console
static void print_link_security(hci_con_handle_t h, const char *context) {
    // A non-zero encryption key size means the LE link is currently encrypted
    uint8_t key_size = gap_encryption_key_size(h);
    printf("[BLE Host] Link 0x%04X security (%s): encrypted=%d key_size=%u authenticated=%d secure_conn=%d bonded=%d\n",
           h, context,
           key_size > 0,
           key_size,
           gap_authenticated(h),
           gap_secure_connection(h),
           gap_bonded(h));
}

void BleHidHost::setStackLogging(bool enable) {
    s_stack_logging = enable;
    if (enable) arm_verbose_auto_off();
    hci_dump_enable_log_level(HCI_DUMP_LOG_LEVEL_INFO, enable ? 1 : 0);
    hci_dump_enable_log_level(HCI_DUMP_LOG_LEVEL_ERROR, 1);
    printf("[BLE Host] BTstack log_info output %s.\n", enable ? "enabled" : "disabled");
}

bool BleHidHost::isStackLogging() {
    return s_stack_logging;
}

void BleHidHost::setAuthReq(bool mitm, bool secure_connections) {
    s_sm_auth_req = SM_AUTHREQ_BONDING;
    if (mitm) s_sm_auth_req |= SM_AUTHREQ_MITM_PROTECTION;
    if (secure_connections) s_sm_auth_req |= SM_AUTHREQ_SECURE_CONNECTION;
    apply_auth_req(false);
    dumpAuthReq();
}

void BleHidHost::dumpAuthReq() {
    printf("[BLE Host] Pairing policy for new pairings: %s, %s (authreq 0x%02X). Existing bonds are unaffected.\n",
           (s_sm_auth_req & SM_AUTHREQ_SECURE_CONNECTION) ? "LE Secure Connections" : "LE legacy pairing",
           (s_sm_auth_req & SM_AUTHREQ_MITM_PROTECTION) ? "MITM/passkey required" : "no MITM (Just Works unless peer insists)",
           s_sm_auth_req);
}

void BleHidHost::init() {
    // Route BTstack's log_info/log_error text through the filtered sink above. Raw HCI packet
    // dumping stays off: it would flood the console and stall the run loop during scanning.
    hci_dump_init(&s_stack_log_sink);
    hci_dump_enable_packet_log(false);
    setStackLogging(BLE_STACK_LOG_DEFAULT != 0);

    l2cap_init();

    // Security Manager setup: Display Only for 6-digit keyboard passkey pairing
    sm_init();
    sm_set_io_capabilities(IO_CAPABILITY_DISPLAY_ONLY);
    apply_auth_req(false);
    dumpAuthReq();

    gatt_client_init();
    att_server_init(s_profile_data, NULL, NULL);

    hids_client_init(s_hid_descriptor_storage, sizeof(s_hid_descriptor_storage));

    ClassicHidHost::init();

    s_hci_event_callback_registration.callback = &packetHandler;
    hci_add_event_handler(&s_hci_event_callback_registration);

    s_sm_event_callback_registration.callback = &smPacketHandler;
    sm_add_event_handler(&s_sm_event_callback_registration);

    // Background scanning: 100ms interval (160), 20ms window (32) for 20% duty cycle
    // to preserve radio bandwidth for active concurrent BLE HID connections.
    gap_set_scan_parameters(1, 160, 32);

    // Initial connection parameters: 30ms scan window/interval, 15-30ms conn interval, 4s supervision timeout
    gap_set_connection_parameters(48, 48, 12, 24, 0, 400, 0, 0);

    // Heartbeat for the watchdog, and from here on console-enabled dumps switch themselves off.
    s_heartbeat_ms = to_ms_since_boot(get_absolute_time());
    s_heartbeat_started = true;
    btstack_run_loop_set_timer_handler(&s_heartbeat_timer, &onHeartbeat);
    btstack_run_loop_set_timer(&s_heartbeat_timer, 1000);
    btstack_run_loop_add_timer(&s_heartbeat_timer);
    s_logging_ready = true;

    for (uint8_t i = 0; i < MAX_BLE_DEVICES; i++) {
        reset_slot(&s_slots[i]);
    }

    s_is_scanning = false;
    s_is_connecting = false;
    s_connecting_slot_idx = -1;
    s_active_passkey = 0;
    s_pending_pairing_handle = HCI_CON_HANDLE_INVALID;

    load_bonded_devices();

    hci_power_control(HCI_POWER_ON);
}

void BleHidHost::startScan() {
    if (s_is_connecting) {
        printf("[BLE Host] Aborting pending connection to start scan...\n");
        gap_connect_cancel();
        return; // GAP_SUBEVENT_LE_CONNECTION_COMPLETE will complete cleanup and restart scanning
    }
    if (s_is_scanning) return;
    printf("[BLE Host] Starting scan for BLE HID peripherals...\n");
    s_is_scanning = true;
    s_active_passkey = 0;
    s_protocol_mode = HID_PROTOCOL_MODE_REPORT;
    gap_start_scan();
}

void BleHidHost::stopScan() {
    if (!s_is_scanning) return;
    printf("[BLE Host] Stopping BLE scan.\n");
    s_is_scanning = false;
    gap_stop_scan();
}

bool BleHidHost::isScanning() {
    return s_is_scanning;
}

void BleHidHost::startPairingMode(uint32_t timeout_ms) {
    s_is_pairing_mode = true;
    apply_auth_req(false);  // every pairing window starts with the configured policy
    s_pairing_mode_start_ms = to_ms_since_boot(get_absolute_time());
    s_pairing_mode_duration_ms = timeout_ms;
    btstack_run_loop_remove_timer(&s_pairing_mode_timer);
    btstack_run_loop_set_timer(&s_pairing_mode_timer, timeout_ms);
    btstack_run_loop_set_timer_handler(&s_pairing_mode_timer, &onPairingModeTimeout);
    btstack_run_loop_add_timer(&s_pairing_mode_timer);
    LogRing::breadcrumb(CRUMB_PAIRING_START);
    printf("[BLE Host] Pairing mode started (%lu s timeout). Unbonded HID peripherals accepted.\n",
           (unsigned long)(timeout_ms / 1000));
    ClassicHidHost::startPairingMode();
    BleHidHost::startScan();
}

void BleHidHost::stopPairingMode() {
    if (!s_is_pairing_mode) return;
    s_is_pairing_mode = false;
    apply_auth_req(false);
    btstack_run_loop_remove_timer(&s_pairing_mode_timer);
    LogRing::breadcrumb(CRUMB_PAIRING_STOP);
    printf("[BLE Host] Pairing mode stopped.\n");
    ClassicHidHost::stopPairingMode();
    if (!BleHidHost::hasUnconnectedBonds()) {
        BleHidHost::stopScan();
    } else {
        BleHidHost::startScan();
    }
}

bool BleHidHost::isPairingMode() {
    return s_is_pairing_mode;
}

uint32_t BleHidHost::getPairingModeRemainingSec() {
    if (!s_is_pairing_mode) return 0;
    uint32_t elapsed = to_ms_since_boot(get_absolute_time()) - s_pairing_mode_start_ms;
    if (elapsed >= s_pairing_mode_duration_ms) return 0;
    return (s_pairing_mode_duration_ms - elapsed + 999) / 1000;
}

bool BleHidHost::isConnected() {
    return (getConnectedCount() > 0);
}

uint8_t BleHidHost::getConnectedCount() {
    uint8_t count = 0;
    for (uint8_t i = 0; i < MAX_BLE_DEVICES; i++) {
        if (s_slots[i].connected) count++;
    }
    return count;
}

uint8_t BleHidHost::getBondedCount() {
    return s_bonded_table.count;
}

bool BleHidHost::hasUnconnectedBonds() {
    for (uint8_t b = 0; b < s_bonded_table.count; b++) {
        bool connected = false;
        for (uint8_t i = 0; i < MAX_BLE_DEVICES; i++) {
            if (s_slots[i].connected && bd_addr_cmp(s_slots[i].addr, s_bonded_table.records[b].addr) == 0) {
                connected = true;
                break;
            }
        }
        if (!connected) return true;
    }
    return false;
}

const char* BleHidHost::getConnectedDeviceName() {
    uint8_t count = getConnectedCount();
    if (count == 0) return "None";
    if (count == 1) {
        for (uint8_t i = 0; i < MAX_BLE_DEVICES; i++) {
            if (s_slots[i].connected) return s_slots[i].name;
        }
    }

    snprintf(s_dev_name_summary, sizeof(s_dev_name_summary), "%u Devs Connected", count);
    return s_dev_name_summary;
}

const char* BleHidHost::getConnectedDeviceName(uint8_t slot_idx) {
    if (slot_idx < MAX_BLE_DEVICES && s_slots[slot_idx].connected) {
        return s_slots[slot_idx].name;
    }
    return "None";
}

bool BleHidHost::getSlotAddress(uint8_t dev_idx, uint8_t addr[6]) {
    if (dev_idx < MAX_BLE_DEVICES && s_slots[dev_idx].connected) {
        memcpy(addr, s_slots[dev_idx].addr, 6);
        return true;
    }
    return false;
}

uint32_t BleHidHost::getActivePasskey() {
    return s_active_passkey;
}

void BleHidHost::clearPasskey() {
    s_active_passkey = 0;
}

void BleHidHost::clearBonds() {
    printf("[BLE Host] Clearing all bonded devices and disconnecting active links.\n");
    btstack_run_loop_remove_timer(&s_pairing_timer);
    btstack_run_loop_remove_timer(&s_reconnect_timer);

    memset(&s_bonded_table, 0, sizeof(s_bonded_table));
    ClassicHidHost::clearBonds();

    const btstack_tlv_t *tlv_impl = nullptr;
    void *tlv_context = nullptr;
    btstack_tlv_get_instance(&tlv_impl, &tlv_context);
    if (tlv_impl) {
        tlv_impl->delete_tag(tlv_context, TLV_TAG_HOG_TABLE);
        tlv_impl->delete_tag(tlv_context, TLV_TAG_HOGD);
    }

    for (int i = 0; i < le_device_db_max_count(); i++) {
        le_device_db_remove(i);
    }

    for (uint8_t i = 0; i < MAX_BLE_DEVICES; i++) {
        if (s_slots[i].con_handle != HCI_CON_HANDLE_INVALID) {
            gap_disconnect(s_slots[i].con_handle);
        }
        Multiplexer::purgeKeyboard(s_slots[i].dev_idx);
        Multiplexer::purgeMouse(s_slots[i].dev_idx);
        reset_slot(&s_slots[i]);
    }

    s_is_connecting = false;
    s_connecting_slot_idx = -1;
    s_active_passkey = 0;

    startScan();
}

void BleHidHost::dumpDevices() {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    printf("[BLE Host] Connected Devices (%u / %u):\n", getConnectedCount(), MAX_BLE_DEVICES);
    for (uint8_t i = 0; i < MAX_BLE_DEVICES; i++) {
        if (s_slots[i].connected) {
            uint32_t idle_s = (now >= s_slots[i].last_used_ms) ? (now - s_slots[i].last_used_ms) / 1000 : 0;
            printf("  Slot %u: '%s' (%s, handle 0x%04X, cid 0x%04X, interval %.2f ms",
                   i, s_slots[i].name, bd_addr_to_str(s_slots[i].addr),
                   s_slots[i].con_handle, s_slots[i].hids_cid,
                   s_slots[i].conn_interval * 1.25f);
            if (s_slots[i].min_conn_interval > 0 && s_slots[i].min_conn_interval != s_slots[i].conn_interval) {
                printf(" [min %.2f ms]", s_slots[i].min_conn_interval * 1.25f);
            }
            printf(", latency %u", s_slots[i].conn_latency);
            if (s_slots[i].has_led_report) {
                printf(", led_id %u", s_slots[i].led_report_id);
            } else if (s_slots[i].led_report_resolved) {
                printf(", no_leds");
            }
            if (s_slots[i].mouse_format != MOUSE_FORMAT_UNKNOWN) {
                const char *mfmt = (s_slots[i].mouse_format == MOUSE_FORMAT_LOGITECH_12BIT) ? "logi-12b" :
                                   (s_slots[i].mouse_format == MOUSE_FORMAT_16BIT) ? "16b" : "8b";
                printf(", mouse %s (speed %u%%)", mfmt, s_slots[i].mouse_speed_percent);
            } else if (s_slots[i].mouse_speed_percent != 100) {
                printf(", speed %u%%", s_slots[i].mouse_speed_percent);
            }
            printf(", idle %lu s)\n", (unsigned long)idle_s);
        }
    }
}

void BleHidHost::setMouseSpeed(uint8_t slot_idx, uint16_t percent) {
    if (slot_idx < MAX_BLE_DEVICES) {
        if (percent == 0) percent = 1;
        if (percent > 1000) percent = 1000;
        s_slots[slot_idx].mouse_speed_percent = percent;
        s_slots[slot_idx].scale_rem_x = 0;
        s_slots[slot_idx].scale_rem_y = 0;
        printf("[BLE Host] Slot %u ('%s') mouse speed set to %u%%\n",
               slot_idx, s_slots[slot_idx].name, percent);
    }
}

uint16_t BleHidHost::getMouseSpeed(uint8_t slot_idx) {
    if (slot_idx < MAX_BLE_DEVICES) {
        return s_slots[slot_idx].mouse_speed_percent;
    }
    return 100;
}

void BleHidHost::setGlobalMouseSpeed(uint16_t percent) {
    if (percent == 0) percent = 1;
    if (percent > 1000) percent = 1000;
    s_global_mouse_speed_percent = percent;
    for (uint8_t i = 0; i < MAX_BLE_DEVICES; i++) {
        s_slots[i].mouse_speed_percent = percent;
        s_slots[i].scale_rem_x = 0;
        s_slots[i].scale_rem_y = 0;
    }
    printf("[BLE Host] Global mouse speed set to %u%% (applied to all %u slots)\n",
           percent, MAX_BLE_DEVICES);
}

uint16_t BleHidHost::getGlobalMouseSpeed() {
    return s_global_mouse_speed_percent;
}

void BleHidHost::dumpBonds() {
    printf("[BLE Host] Bonded Devices Table (%u stored):\n", s_bonded_table.count);
    for (uint8_t i = 0; i < s_bonded_table.count; i++) {
        printf("  [%u] '%s' (%s, type %u)\n",
               i, s_bonded_table.records[i].name,
               bd_addr_to_str(s_bonded_table.records[i].addr),
               (unsigned)s_bonded_table.records[i].addr_type);
    }
    printf("[BLE Host] le_device_db (max %d):\n", le_device_db_max_count());
    int valid = 0;
    for (int i = 0; i < le_device_db_max_count(); i++) {
        int addr_type = 0;
        bd_addr_t addr;
        le_device_db_info(i, &addr_type, addr, nullptr);
        if (addr_type != BD_ADDR_TYPE_UNKNOWN) {
            printf("  Slot %d: %s (type %u)\n", i, bd_addr_to_str(addr), (unsigned)addr_type);
            valid++;
        }
    }
    if (valid == 0) {
        printf("  (no bonded keys in le_device_db)\n");
    }
}

// Scan HID Report Descriptor for an Output item under Usage Page 0x08 (LEDs)
static bool find_led_output_report_id(const uint8_t *desc, uint16_t desc_len, uint8_t *out_report_id) {
    if (!desc || desc_len == 0) return false;

    struct GlobalState {
        uint32_t usage_page;
        uint8_t report_id;
    };
    GlobalState current_state = {0, 0};
    GlobalState state_stack[4];
    uint8_t stack_depth = 0;

    uint16_t i = 0;
    while (i < desc_len) {
        uint8_t b = desc[i++];
        if (b == 0xFE) { // Long item: 0xFE, bDataSize, bLongItemTag, data...
            if (i + 2 > desc_len) break;
            uint8_t data_len = desc[i];
            i += 2 + data_len;
            continue;
        }

        uint8_t bTag = (b >> 4) & 0x0F;
        uint8_t bType = (b >> 2) & 0x03;
        uint8_t bSize = b & 0x03;
        if (bSize == 3) bSize = 4;
        if (i + bSize > desc_len) break;

        uint32_t val = 0;
        for (uint8_t j = 0; j < bSize; j++) {
            val |= ((uint32_t)desc[i + j]) << (8 * j);
        }
        i += bSize;

        if (bType == 1) { // Global item
            if (bTag == 0) { // Usage Page
                current_state.usage_page = val;
            } else if (bTag == 8) { // Report ID
                current_state.report_id = (uint8_t)val;
            } else if (bTag == 10) { // Push
                if (stack_depth < 4) {
                    state_stack[stack_depth++] = current_state;
                }
            } else if (bTag == 11) { // Pop
                if (stack_depth > 0) {
                    current_state = state_stack[--stack_depth];
                }
            }
        } else if (bType == 0) { // Main item
            if (bTag == 9) { // Output
                if (current_state.usage_page == 0x08) { // Usage Page: LEDs
                    if (out_report_id) {
                        *out_report_id = current_state.report_id;
                    }
                    return true;
                }
            }
        }
    }
    return false;
}

static void resolve_led_report(BleSlot *slot) {
    if (!slot || slot->hids_cid == 0) return;
    const uint8_t *desc = hids_client_descriptor_storage_get_descriptor_data(slot->hids_cid, 0);
    uint16_t desc_len = hids_client_descriptor_storage_get_descriptor_len(slot->hids_cid, 0);
    if (!desc || desc_len == 0) return;

    uint8_t led_id = 0;
    if (find_led_output_report_id(desc, desc_len, &led_id)) {
        slot->has_led_report = true;
        slot->led_report_id = led_id;
        slot->led_report_resolved = true;
        printf("[BLE Host] Slot %u '%s': detected LED output report ID %u\n",
               slot->dev_idx, slot->name, led_id);
    } else {
        slot->has_led_report = false;
        slot->led_report_resolved = true;
        printf("[BLE Host] Slot %u '%s': no LED output report found in descriptor\n",
               slot->dev_idx, slot->name);
    }
}

// Scan HID Report Descriptor for Mouse Input item (Generic Desktop 0x01, Usage 0x30 X or 0x31 Y)
// and determine coordinate bit size (12-bit packed vs 16-bit vs 8-bit)
static bool find_mouse_format_and_report_id(const uint8_t *desc, uint16_t desc_len,
                                            uint8_t *out_report_id, BleMouseFormat *out_format) {
    if (!desc || desc_len == 0) return false;

    struct GlobalState {
        uint32_t usage_page;
        uint8_t report_id;
        uint8_t report_size;
        uint8_t report_count;
    };
    GlobalState current_state = {0, 0, 0, 0};
    GlobalState state_stack[4];
    uint8_t stack_depth = 0;

    bool found_xy = false;

    uint16_t i = 0;
    while (i < desc_len) {
        uint8_t b = desc[i++];
        if (b == 0xFE) { // Long item
            if (i + 2 > desc_len) break;
            uint8_t data_len = desc[i];
            i += 2 + data_len;
            continue;
        }

        uint8_t bTag = (b >> 4) & 0x0F;
        uint8_t bType = (b >> 2) & 0x03;
        uint8_t bSize = b & 0x03;
        if (bSize == 3) bSize = 4;
        if (i + bSize > desc_len) break;

        uint32_t val = 0;
        for (uint8_t j = 0; j < bSize; j++) {
            val |= ((uint32_t)desc[i + j]) << (8 * j);
        }
        i += bSize;

        if (bType == 1) { // Global item
            if (bTag == 0) { // Usage Page
                current_state.usage_page = val;
            } else if (bTag == 7) { // Report Size
                current_state.report_size = (uint8_t)val;
            } else if (bTag == 8) { // Report ID
                current_state.report_id = (uint8_t)val;
            } else if (bTag == 9) { // Report Count
                current_state.report_count = (uint8_t)val;
            } else if (bTag == 10) { // Push
                if (stack_depth < 4) {
                    state_stack[stack_depth++] = current_state;
                }
            } else if (bTag == 11) { // Pop
                if (stack_depth > 0) {
                    current_state = state_stack[--stack_depth];
                }
            }
        } else if (bType == 2) { // Local item
            if (bTag == 0) { // Usage
                if (current_state.usage_page == 0x01 && (val == 0x30 || val == 0x31)) {
                    found_xy = true;
                }
            }
        } else if (bType == 0) { // Main item
            if (bTag == 8) { // Input
                if (found_xy && current_state.usage_page == 0x01) {
                    if (out_report_id) {
                        *out_report_id = current_state.report_id;
                    }
                    if (out_format) {
                        if (current_state.report_size == 12) {
                            *out_format = MOUSE_FORMAT_LOGITECH_12BIT;
                        } else if (current_state.report_size == 16) {
                            *out_format = MOUSE_FORMAT_16BIT;
                        } else {
                            *out_format = MOUSE_FORMAT_STANDARD;
                        }
                    }
                    return true;
                }
                found_xy = false;
            }
        }
    }
    return false;
}

static void resolve_mouse_format(BleSlot *slot) {
    if (!slot) return;
    if (slot->hids_cid == 0) {
        if (strstr(slot->name, "LIFT") || strstr(slot->name, "Logi") || strstr(slot->name, "MX ")) {
            slot->mouse_format = MOUSE_FORMAT_LOGITECH_12BIT;
            slot->mouse_report_id = 2;
        }
        return;
    }

    const uint8_t *desc = hids_client_descriptor_storage_get_descriptor_data(slot->hids_cid, 0);
    uint16_t desc_len = hids_client_descriptor_storage_get_descriptor_len(slot->hids_cid, 0);
    if (!desc || desc_len == 0) {
        if (strstr(slot->name, "LIFT") || strstr(slot->name, "Logi") || strstr(slot->name, "MX ")) {
            slot->mouse_format = MOUSE_FORMAT_LOGITECH_12BIT;
            slot->mouse_report_id = 2;
        }
        return;
    }

    uint8_t rep_id = 0;
    BleMouseFormat fmt = MOUSE_FORMAT_UNKNOWN;
    if (find_mouse_format_and_report_id(desc, desc_len, &rep_id, &fmt)) {
        slot->mouse_format = fmt;
        slot->mouse_report_id = rep_id;
        slot->mouse_format_resolved = true;
        const char *fmt_name = (fmt == MOUSE_FORMAT_LOGITECH_12BIT) ? "Logitech 12-bit packed" :
                               (fmt == MOUSE_FORMAT_16BIT) ? "16-bit relative" : "Standard 8-bit";
        printf("[BLE Host] Slot %u '%s': detected Mouse report (ID %u, format: %s)\n",
               slot->dev_idx, slot->name, rep_id, fmt_name);
    } else {
        slot->mouse_format_resolved = true;
    }
}

void BleHidHost::sendHostLeds(uint8_t leds) {
    uint8_t led_report = leds;
    for (uint8_t i = 0; i < MAX_BLE_DEVICES; i++) {
        if (s_slots[i].connected && s_slots[i].hids_cid != 0) {
            if (!s_slots[i].led_report_resolved) {
                resolve_led_report(&s_slots[i]);
            }
            if (s_slots[i].has_led_report) {
                uint8_t status = hids_client_send_write_report(s_slots[i].hids_cid, s_slots[i].led_report_id,
                                                               HID_REPORT_TYPE_OUTPUT, &led_report, 1);
                if (status != ERROR_CODE_SUCCESS && s_stack_logging) {
                    printf("[BLE Host] Slot %u: sendHostLeds(id %u) status 0x%02X\n",
                           i, s_slots[i].led_report_id, status);
                }
            }
        }
    }
}

void BleHidHost::dumpDescriptor() {
    uint8_t count = getConnectedCount();
    if (count == 0) {
        printf("[BLE Host] No active HIDS connections to dump descriptor.\n");
        return;
    }
    for (uint8_t i = 0; i < MAX_BLE_DEVICES; i++) {
        if (s_slots[i].connected && s_slots[i].hids_cid != 0) {
            const uint8_t *desc = hids_client_descriptor_storage_get_descriptor_data(s_slots[i].hids_cid, 0);
            uint16_t desc_len = hids_client_descriptor_storage_get_descriptor_len(s_slots[i].hids_cid, 0);
            printf("[BLE Host] Slot %u '%s' HID Report Descriptor (len %u):\n", i, s_slots[i].name, desc_len);
            if (desc && desc_len > 0) {
                printf_hexdump(desc, desc_len);
            }
        }
    }
}

void BleHidHost::sendExitSuspend(uint8_t slot_idx) {
    if (slot_idx >= MAX_BLE_DEVICES || !s_slots[slot_idx].connected || s_slots[slot_idx].hids_cid == 0) {
        printf("[BLE Host] Slot %u not connected or no HIDS CID.\n", slot_idx);
        return;
    }
    uint8_t status = hids_client_send_exit_suspend(s_slots[slot_idx].hids_cid, 0);
    printf("[BLE Host] Slot %u: send_exit_suspend status 0x%02X\n", slot_idx, status);
}

void BleHidHost::sendSetProtocolMode(uint8_t slot_idx, uint8_t mode) {
    if (slot_idx >= MAX_BLE_DEVICES || !s_slots[slot_idx].connected || s_slots[slot_idx].hids_cid == 0) {
        printf("[BLE Host] Slot %u not connected or no HIDS CID.\n", slot_idx);
        return;
    }
    uint8_t status = hids_client_send_set_protocol_mode(s_slots[slot_idx].hids_cid, 0, (hid_protocol_mode_t)mode);
    printf("[BLE Host] Slot %u: send_set_protocol_mode(%u) status 0x%02X\n", slot_idx, mode, status);
}

void BleHidHost::sendGetReport(uint8_t slot_idx, uint8_t report_id) {
    if (slot_idx >= MAX_BLE_DEVICES || !s_slots[slot_idx].connected || s_slots[slot_idx].hids_cid == 0) {
        printf("[BLE Host] Slot %u not connected or no HIDS CID.\n", slot_idx);
        return;
    }
    uint8_t status = hids_client_send_get_report(s_slots[slot_idx].hids_cid, report_id, HID_REPORT_TYPE_INPUT);
    printf("[BLE Host] Slot %u: send_get_report(id %u) status 0x%02X\n", slot_idx, report_id, status);
}

void BleHidHost::enableNotifications(uint8_t slot_idx) {
    if (slot_idx >= MAX_BLE_DEVICES || !s_slots[slot_idx].connected || s_slots[slot_idx].hids_cid == 0) {
        printf("[BLE Host] Slot %u not connected or no HIDS CID.\n", slot_idx);
        return;
    }
    uint8_t status = hids_client_enable_notifications(s_slots[slot_idx].hids_cid);
    printf("[BLE Host] Slot %u: enable_notifications status 0x%02X\n", slot_idx, status);
}

void BleHidHost::requestProtocolMode(uint8_t slot_idx) {
    if (slot_idx >= MAX_BLE_DEVICES || !s_slots[slot_idx].connected || s_slots[slot_idx].hids_cid == 0) {
        printf("[BLE Host] Slot %u not connected or no HIDS CID.\n", slot_idx);
        return;
    }
    // Result arrives as GATTSERVICE_SUBEVENT_HID_PROTOCOL_MODE
    uint8_t status = hids_client_get_protocol_mode(s_slots[slot_idx].hids_cid, 0);
    printf("[BLE Host] Slot %u: get_protocol_mode status 0x%02X\n", slot_idx, status);
}

void BleHidHost::updateConnectionParams(uint8_t slot_idx, uint16_t interval_units, uint16_t latency) {
    if (slot_idx >= MAX_BLE_DEVICES || s_slots[slot_idx].con_handle == HCI_CON_HANDLE_INVALID) {
        printf("[BLE Host] Slot %u has no active connection.\n", slot_idx);
        return;
    }
    BleSlot *slot = &s_slots[slot_idx];
    if (interval_units == 0) {
        // Fall back to the shortest recorded interval (or 7.5 ms if unknown)
        interval_units = slot->min_conn_interval ? slot->min_conn_interval : (slot->conn_interval ? slot->conn_interval : 6);
    }
    // Supervision timeout (10 ms units) must exceed (1 + latency) * interval * 2; keep 4 s unless
    // the requested latency needs more.
    uint32_t min_timeout = ((1u + latency) * interval_units * 125u * 2u) / 1000u + 10u;
    uint16_t timeout = (min_timeout > 400u) ? (uint16_t)min_timeout : 400u;
    int status = gap_update_connection_parameters(slot->con_handle, interval_units, interval_units, latency, timeout);
    printf("[BLE Host] Slot %u: connection parameter update requested (interval %.2f ms, latency %u, timeout %u ms): status %d\n",
           slot_idx, interval_units * 1.25f, latency, timeout * 10, status);
}

void BleHidHost::disconnectSlot(uint8_t slot_idx) {
    if (slot_idx >= MAX_BLE_DEVICES || s_slots[slot_idx].con_handle == HCI_CON_HANDLE_INVALID) {
        printf("[BLE Host] Slot %u has no active connection.\n", slot_idx);
        return;
    }
    printf("[BLE Host] Disconnecting slot %u (handle 0x%04X)...\n", slot_idx, s_slots[slot_idx].con_handle);
    gap_disconnect(s_slots[slot_idx].con_handle);
}

void BleHidHost::unbond(uint8_t idx) {
    if (idx >= s_bonded_table.count) {
        printf("[BLE Host] Invalid bond index %u (stored: %u)\n", idx, s_bonded_table.count);
        return;
    }
    bd_addr_t target_addr;
    bd_addr_copy(target_addr, s_bonded_table.records[idx].addr);
    char name[32];
    strncpy(name, s_bonded_table.records[idx].name, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';

    BleSlot *slot = find_slot_by_addr(target_addr);
    if (slot && slot->con_handle != HCI_CON_HANDLE_INVALID) {
        gap_disconnect(slot->con_handle);
    }

    for (int i = 0; i < le_device_db_max_count(); i++) {
        int db_type = 0;
        bd_addr_t db_addr;
        le_device_db_info(i, &db_type, db_addr, nullptr);
        if (db_type != BD_ADDR_TYPE_UNKNOWN && bd_addr_cmp(target_addr, db_addr) == 0) {
            le_device_db_remove(i);
            printf("[BLE Host] Removed bond from le_device_db slot %d (%s)\n", i, bd_addr_to_str(target_addr));
            break;
        }
    }

    for (uint8_t i = idx; i < s_bonded_table.count - 1; i++) {
        s_bonded_table.records[i] = s_bonded_table.records[i + 1];
    }
    s_bonded_table.count--;
    save_bonded_devices();
    printf("[BLE Host] Unbonded [%u] '%s' (%s). %u bonded device(s) remaining.\n",
           idx, name, bd_addr_to_str(target_addr), s_bonded_table.count);
}

// Applies sensitivity scaling with fractional remainder accumulation to avoid dropping sub-count movements
static inline void scale_mouse_delta(BleSlot *slot, int16_t &dx, int16_t &dy) {
    if (slot && slot->mouse_speed_percent != 100) {
        int32_t scaled_x = (int32_t)dx * slot->mouse_speed_percent + slot->scale_rem_x;
        dx = (int16_t)(scaled_x / 100);
        slot->scale_rem_x = (int16_t)(scaled_x % 100);

        int32_t scaled_y = (int32_t)dy * slot->mouse_speed_percent + slot->scale_rem_y;
        dy = (int16_t)(scaled_y / 100);
        slot->scale_rem_y = (int16_t)(scaled_y % 100);
    }
}

void BleHidHost::gattPacketHandler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET && packet_type != HCI_EVENT_GATTSERVICE_META) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_GATTSERVICE_META) return;

    uint8_t subevent = hci_event_gattservice_meta_get_subevent_code(packet);
    if (subevent != GATTSERVICE_SUBEVENT_HID_REPORT) {  // reports are far too frequent to trace
        LogRing::breadcrumb(CRUMB_GATT(subevent));
    }
    switch (subevent) {
        case GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED: {
            uint16_t cid = gattservice_subevent_hid_service_connected_get_hids_cid(packet);
            uint8_t status = gattservice_subevent_hid_service_connected_get_status(packet);
            BleSlot *slot = find_slot_by_cid(cid);

            if (status == ERROR_CODE_SUCCESS) {
                uint8_t mode = gattservice_subevent_hid_service_connected_get_protocol_mode(packet);
                uint8_t instances = gattservice_subevent_hid_service_connected_get_num_instances(packet);
                printf("[BLE Host] HID Service Connected successfully! (slot %u, cid 0x%04X, mode: %u, instances: %u)\n",
                       slot ? slot->dev_idx : 0xFF, cid, mode, instances);

                if (slot) {
                    slot->connected = true;
                    s_active_passkey = 0;

                    add_or_update_bonded_device(slot->addr, slot->addr_type, slot->name);

                    resolve_led_report(slot);
                    resolve_mouse_format(slot);
                    if (slot->has_led_report) {
                        uint8_t current_leds = Multiplexer::getHostLeds();
                        hids_client_send_write_report(slot->hids_cid, slot->led_report_id, HID_REPORT_TYPE_OUTPUT, &current_leds, 1);
                    }

                    // In Report mode BTstack has already written every input report's CCCD before
                    // emitting this event, so no extra enable_notifications() call is needed here. Doing
                    // it anyway rewrites all CCCDs and keeps the HIDS client busy (every other request
                    // returns COMMAND_DISALLOWED) for as long as the peripheral takes to answer, which on
                    // the ProtoArc XK01 is a 30 s GATT timeout per write. 'notif <slot>' remains for manual use.

                    // If pairing mode was active, exit now that the new device has paired and connected.
                    // Otherwise keep looking for bonded devices that are still missing; once every bonded
                    // device is connected, stop scanning so it no longer takes radio time from the
                    // links (the main loop restarts it when a device drops off).
                    if (s_is_pairing_mode) {
                        printf("[BLE Host] Peripheral paired and connected successfully. Exiting pairing mode.\n");
                        BleHidHost::stopPairingMode();
                    } else if (BleHidHost::hasUnconnectedBonds() && BleHidHost::getConnectedCount() < MAX_BLE_DEVICES) {
                        BleHidHost::startScan();
                    } else {
                        BleHidHost::stopScan();
                    }
                }
            } else {
                printf("[BLE Host] HID Service connection failed for cid 0x%04X, status: 0x%02X\n", cid, status);
                if (slot) {
                    slot->connected = false;
                    if (slot->con_handle != HCI_CON_HANDLE_INVALID) {
                        gap_disconnect(slot->con_handle);
                    }
                }
                tryAutoReconnectOrScan();
            }
            break;
        }

        case GATTSERVICE_SUBEVENT_HID_REPORT_WRITTEN: {
            if (s_stack_logging) {
                uint16_t cid = gattservice_subevent_hid_report_written_get_hids_cid(packet);
                uint8_t rep_id = gattservice_subevent_hid_report_written_get_report_id(packet);
                BleSlot *slot = find_slot_by_cid(cid);
                printf("[BLE Host] HID Report Written (slot %u, cid 0x%04X, id %u)\n",
                       slot ? slot->dev_idx : 0xFF, cid, rep_id);
            }
            break;
        }

        case GATTSERVICE_SUBEVENT_HID_SERVICE_REPORTS_NOTIFICATION: {
            uint16_t cid = gattservice_subevent_hid_service_reports_notification_get_hids_cid(packet);
            BleSlot *slot = find_slot_by_cid(cid);
            printf("[BLE Host] HID Reports notification configuration done (slot %u, cid 0x%04X).\n",
                   slot ? slot->dev_idx : 0xFF, cid);
            break;
        }

        case GATTSERVICE_SUBEVENT_HID_INFORMATION: {
            uint16_t cid = gattservice_subevent_hid_information_get_hids_cid(packet);
            BleSlot *slot = find_slot_by_cid(cid);
            printf("[BLE Host] HID Information (slot %u): bcdHID 0x%04X, country %u, remote_wake %u, normally_connectable %u\n",
                   slot ? slot->dev_idx : 0xFF,
                   gattservice_subevent_hid_information_get_base_usb_hid_version(packet),
                   gattservice_subevent_hid_information_get_country_code(packet),
                   gattservice_subevent_hid_information_get_remote_wake(packet),
                   gattservice_subevent_hid_information_get_normally_connectable(packet));
            break;
        }

        case GATTSERVICE_SUBEVENT_HID_PROTOCOL_MODE: {
            uint16_t cid = gattservice_subevent_hid_protocol_mode_get_hids_cid(packet);
            BleSlot *slot = find_slot_by_cid(cid);
            uint8_t mode = gattservice_subevent_hid_protocol_mode_get_protocol_mode(packet);
            printf("[BLE Host] Protocol Mode read (slot %u): %u (%s)\n",
                   slot ? slot->dev_idx : 0xFF, mode, mode == 0 ? "Boot" : mode == 1 ? "Report" : "?");
            break;
        }

        case GATTSERVICE_SUBEVENT_HID_SERVICE_DISCONNECTED: {
            uint16_t cid = gattservice_subevent_hid_service_disconnected_get_hids_cid(packet);
            BleSlot *slot = find_slot_by_cid(cid);
            printf("[BLE Host] HID Service Disconnected (cid 0x%04X, slot %u).\n", cid, slot ? slot->dev_idx : 0xFF);
            if (slot) {
                slot->connected = false;
                slot->hids_cid = 0;
                slot->led_report_resolved = false;
                slot->has_led_report = false;
                slot->led_report_id = 0;
                Multiplexer::purgeKeyboard(slot->dev_idx);
                Multiplexer::purgeMouse(slot->dev_idx);
            }
            tryAutoReconnectOrScan();
            break;
        }

        case GATTSERVICE_SUBEVENT_HID_REPORT: {
            uint16_t cid = gattservice_subevent_hid_report_get_hids_cid(packet);
            BleSlot *slot = find_slot_by_cid(cid);
            if (!slot) {
                printf("[BLE Host] REPORT on unknown cid 0x%04X\n", cid);
                break;
            }
            slot->last_used_ms = to_ms_since_boot(get_absolute_time());
            // BTstack registers the notification listener for each input report as soon as its CCCD
            // write is issued, well before it emits HID_SERVICE_CONNECTED for the whole service. A
            // keyboard whose remaining CCCD writes are slow (or time out) can therefore deliver valid
            // reports for a long while before the slot is marked connected, so accept them here.
            if (!slot->connected) {
                slot->connected = true;
            }
            if (!slot->led_report_resolved) {
                resolve_led_report(slot);
            }
            if (!slot->mouse_format_resolved) {
                resolve_mouse_format(slot);
            }

            const uint8_t *report = gattservice_subevent_hid_report_get_report(packet);
            uint16_t len = gattservice_subevent_hid_report_get_report_len(packet);
            uint8_t report_id = gattservice_subevent_hid_report_get_report_id(packet);
            if (!report || len == 0) break;

            const uint8_t *data = report;
            uint16_t data_len = len;
            if (len > 1 && report[0] == report_id) {
                data = &report[1];
                data_len = len - 1;
            }

            uint8_t dev_idx = slot->dev_idx;

            // Raw report dump is opt-in ('reports on'): a trackpad emits ~100 reports/s and each
            // console line blocks the BTstack context for several ms on the UART.
            bool log_this_report = s_log_reports && report_log_allowed();
            if (log_this_report) {
                printf("[BLE Host] REPORT slot %u ('%s'): id=%u len=%u [", dev_idx, slot->name, report_id, data_len);
                for (uint16_t i = 0; i < data_len && i < 8; i++) {
                    printf(" %02X", data[i]);
                }
                printf(" ]\n");
            }

            // The mouse report ID comes from the device's own report descriptor, so it takes
            // precedence over the usual ID guesses: some mice (e.g. the Keychron M5 8K) send their
            // mouse reports as ID 1, which would otherwise be taken for keyboard reports and turn
            // every motion into random key presses. Without a known mouse ID, ID 2 is assumed to be
            // the mouse, as before.
            bool is_mouse_report = (slot->mouse_report_id != 0)
                                   ? (report_id == slot->mouse_report_id)
                                   : (report_id == 2);

            if (!is_mouse_report && (report_id == 1 || (report_id == 0 && (data_len == 8 || data_len == 7)))) {
                if (data_len == 8) {
                    Multiplexer::handleKeyboardReport(dev_idx, data[0], &data[2], 6);
                } else if (data_len == 7) {
                    Multiplexer::handleKeyboardReport(dev_idx, data[0], &data[1], 6);
                }
                // Per-keystroke print is diagnostic only ('reports on'): it blocks the BTstack
                // context on the UART for several ms and adds that directly to key latency.
                if (log_this_report && (data[0] != 0 || (data_len >= 3 && data[2] != 0))) {
                    printf("[BLE Host] Key press on slot %u ('%s'): mod=0x%02X key=0x%02X\n",
                           dev_idx, slot->name, data[0], (data_len == 8 ? data[2] : data[1]));
                }
            } else if (is_mouse_report) {
                if (slot->mouse_format == MOUSE_FORMAT_LOGITECH_12BIT ||
                    (slot->mouse_format == MOUSE_FORMAT_UNKNOWN && (strstr(slot->name, "LIFT") || strstr(slot->name, "Logi") || strstr(slot->name, "MX ")))) {
                    // Logitech 12-bit packed mouse report:
                    // data[0]: buttons 1-8, data[1]: buttons 9-16
                    // data[2..4]: 12-bit packed X and Y
                    // data[5]: wheel, data[6]: pan (tilt/h-wheel)
                    if (data_len >= 5) {
                        uint8_t buttons = data[0];
                        uint16_t x_raw = (uint16_t)data[2] | (((uint16_t)(data[3] & 0x0F)) << 8);
                        int16_t dx = (x_raw & 0x0800) ? (int16_t)(x_raw | 0xF000) : (int16_t)x_raw;
                        uint16_t y_raw = ((uint16_t)(data[3] >> 4)) | (((uint16_t)data[4]) << 4);
                        int16_t dy = (y_raw & 0x0800) ? (int16_t)(y_raw | 0xF000) : (int16_t)y_raw;
                        int8_t wheel = (data_len >= 6) ? (int8_t)data[5] : 0;
                        int8_t pan   = (data_len >= 7) ? (int8_t)data[6] : 0;
                        scale_mouse_delta(slot, dx, dy);
                        Multiplexer::handleMouseReport(dev_idx, buttons, dx, dy, wheel, pan);
                    }
                } else if (data_len == 9) {
                    // Keychron Nape Pro 16-bit mouse/trackball with 16-bit wheel and pan
                    uint8_t buttons = data[0];
                    int16_t dx    = (int16_t)((uint16_t)data[1] | ((uint16_t)data[2] << 8));
                    int16_t dy    = (int16_t)((uint16_t)data[3] | ((uint16_t)data[4] << 8));
                    int16_t wheel = (int16_t)((uint16_t)data[5] | ((uint16_t)data[6] << 8));
                    int16_t pan   = (int16_t)((uint16_t)data[7] | ((uint16_t)data[8] << 8));
                    scale_mouse_delta(slot, dx, dy);
                    Multiplexer::handleMouseReport(dev_idx, buttons, dx, dy, (int8_t)wheel, (int8_t)pan);
                } else if (data_len == 7) {
                    // ProtoArc 16-bit relative trackpad: [buttons, dx_l, dx_h, dy_l, dy_h, wheel, pan]
                    uint8_t buttons = data[0];
                    int16_t dx = (int16_t)((uint16_t)data[1] | ((uint16_t)data[2] << 8));
                    int16_t dy = (int16_t)((uint16_t)data[3] | ((uint16_t)data[4] << 8));
                    int8_t wheel = (int8_t)data[5];
                    int8_t pan = (int8_t)data[6];
                    scale_mouse_delta(slot, dx, dy);
                    Multiplexer::handleMouseReport(dev_idx, buttons, dx, dy, wheel, pan);
                } else if (data_len == 6) {
                    uint8_t buttons = data[0];
                    int16_t dx = (int16_t)((uint16_t)data[1] | ((uint16_t)data[2] << 8));
                    int16_t dy = (int16_t)((uint16_t)data[3] | ((uint16_t)data[4] << 8));
                    int8_t wheel = (int8_t)data[5];
                    scale_mouse_delta(slot, dx, dy);
                    Multiplexer::handleMouseReport(dev_idx, buttons, dx, dy, wheel, 0);
                } else if (data_len >= 3 && data_len <= 5) {
                    uint8_t buttons = data[0];
                    int16_t dx = (int8_t)data[1];
                    int16_t dy = (int8_t)data[2];
                    int8_t wheel = (data_len >= 4) ? (int8_t)data[3] : 0;
                    int8_t pan   = (data_len >= 5) ? (int8_t)data[4] : 0;
                    scale_mouse_delta(slot, dx, dy);
                    Multiplexer::handleMouseReport(dev_idx, buttons, dx, dy, wheel, pan);
                }
            } else if (data_len == 9) {
                uint8_t buttons = data[0];
                int16_t dx    = (int16_t)((uint16_t)data[1] | ((uint16_t)data[2] << 8));
                int16_t dy    = (int16_t)((uint16_t)data[3] | ((uint16_t)data[4] << 8));
                int16_t wheel = (int16_t)((uint16_t)data[5] | ((uint16_t)data[6] << 8));
                int16_t pan   = (int16_t)((uint16_t)data[7] | ((uint16_t)data[8] << 8));
                scale_mouse_delta(slot, dx, dy);
                Multiplexer::handleMouseReport(dev_idx, buttons, dx, dy, (int8_t)wheel, (int8_t)pan);
            } else if (data_len == 8) {
                Multiplexer::handleKeyboardReport(dev_idx, data[0], &data[2], 6);
            } else if (data_len >= 3 && data_len <= 5) {
                uint8_t buttons = data[0];
                int16_t dx = (int8_t)data[1];
                int16_t dy = (int8_t)data[2];
                int8_t wheel = (data_len >= 4) ? (int8_t)data[3] : 0;
                int8_t pan   = (data_len >= 5) ? (int8_t)data[4] : 0;
                scale_mouse_delta(slot, dx, dy);
                Multiplexer::handleMouseReport(dev_idx, buttons, dx, dy, wheel, pan);
            } else {
                printf("[BLE Host] Unhandled HID Report for slot %u (id %u, data_len %u)\n", dev_idx, report_id, data_len);
            }
            break;
        }

        default:
            break;
    }
}

void BleHidHost::smPacketHandler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;

    uint8_t sm_event = hci_event_packet_get_type(packet);
    LogRing::breadcrumb(CRUMB_SM(sm_event));

    hci_con_handle_t connect_hids_handle = HCI_CON_HANDLE_INVALID;

    switch (sm_event) {
        case SM_EVENT_JUST_WORKS_REQUEST: {
            hci_con_handle_t h = sm_event_just_works_request_get_handle(packet);
            printf("[BLE Host] Just Works pairing requested for handle 0x%04X. Confirming...\n", h);
            sm_just_works_confirm(h);
            break;
        }

        case SM_EVENT_NUMERIC_COMPARISON_REQUEST: {
            hci_con_handle_t h = sm_event_numeric_comparison_request_get_handle(packet);
            printf("[BLE Host] Numeric comparison confirmed for handle 0x%04X.\n", h);
            sm_numeric_comparison_confirm(h);
            break;
        }

        case SM_EVENT_PASSKEY_DISPLAY_NUMBER:
            s_active_passkey = sm_event_passkey_display_number_get_passkey(packet);
            printf("[BLE Host] >>> PAIRING PASSKEY: %06lu <<< (type it on the keyboard and press Enter)\n",
                   (unsigned long)s_active_passkey);
            break;

        case SM_EVENT_PASSKEY_DISPLAY_CANCEL:
            printf("[BLE Host] Passkey display cancelled.\n");
            s_active_passkey = 0;
            break;

        case SM_EVENT_PAIRING_STARTED: {
            hci_con_handle_t h = sm_event_pairing_started_get_handle(packet);
            printf("[BLE Host] Pairing started for handle 0x%04X (our authreq 0x%02X, %s).\n", h,
                   effective_auth_req(),
                   (effective_auth_req() & SM_AUTHREQ_SECURE_CONNECTION) ? "LE Secure Connections" : "LE legacy pairing");
            break;
        }

        case SM_EVENT_REENCRYPTION_STARTED: {
            hci_con_handle_t h = sm_event_reencryption_started_get_handle(packet);
            printf("[BLE Host] Re-encryption with stored LTK started for handle 0x%04X.\n", h);
            break;
        }

        case SM_EVENT_IDENTITY_RESOLVING_SUCCEEDED: {
            bd_addr_t id_addr;
            sm_event_identity_resolving_succeeded_get_identity_address(packet, id_addr);
            printf("[BLE Host] Identity resolved to %s (le_device_db index %u).\n",
                   bd_addr_to_str(id_addr), sm_event_identity_resolving_succeeded_get_index(packet));
            break;
        }

        case SM_EVENT_PAIRING_COMPLETE: {
            hci_con_handle_t h = sm_event_pairing_complete_get_handle(packet);
            uint8_t status = sm_event_pairing_complete_get_status(packet);
            uint8_t reason = sm_event_pairing_complete_get_reason(packet);
            s_active_passkey = 0;

            BleSlot *slot = find_slot_by_handle(h);
            if (status == ERROR_CODE_SUCCESS) {
                printf("[BLE Host] Pairing complete for handle 0x%04X: SUCCESS\n", h);
                print_link_security(h, "after pairing");
                if (slot) {
                    add_or_update_bonded_device(slot->addr, slot->addr_type, slot->name);
                }
                connect_hids_handle = h;
            } else {
                printf("[BLE Host] Pairing complete for handle 0x%04X: FAILED (status 0x%02X, reason 0x%02X)\n", h, status, reason);
                if (s_is_pairing_mode) {
                    // Some peripherals accept only one of the two methods and just drop the link
                    // otherwise; offer the other one on the next attempt.
                    apply_auth_req(!s_sc_flipped);
                    printf("[BLE Host] Next pairing attempt offers %s.\n",
                           (effective_auth_req() & SM_AUTHREQ_SECURE_CONNECTION) ? "LE Secure Connections" : "LE legacy pairing");
                }
                if (slot) {
                    gap_disconnect(h);
                }
                if (BleHidHost::getConnectedCount() < MAX_BLE_DEVICES) {
                    BleHidHost::startScan();
                }
            }
            break;
        }

        case SM_EVENT_REENCRYPTION_COMPLETE: {
            hci_con_handle_t h = sm_event_reencryption_complete_get_handle(packet);
            uint8_t status = sm_event_reencryption_complete_get_status(packet);
            if (status == ERROR_CODE_SUCCESS) {
                printf("[BLE Host] Re-encryption complete for handle 0x%04X: SUCCESS\n", h);
                print_link_security(h, "after re-encryption");
                connect_hids_handle = h;
            } else {
                printf("[BLE Host] Re-encryption failed for handle 0x%04X (status 0x%02X). Requesting fresh pairing...\n", h, status);
                // Invalidate any stale LTK key in le_device_db for this address so the peripheral
                // can perform a clean fresh SMP pairing exchange without key collision.
                BleSlot *slot = find_slot_by_handle(h);
                if (slot) {
                    for (int i = 0; i < le_device_db_max_count(); i++) {
                        int db_type = 0;
                        bd_addr_t db_addr;
                        le_device_db_info(i, &db_type, db_addr, nullptr);
                        if (db_type != BD_ADDR_TYPE_UNKNOWN && bd_addr_cmp(slot->addr, db_addr) == 0) {
                            printf("[BLE Host] Purged stale bond key from le_device_db slot %d (%s)\n", i, bd_addr_to_str(db_addr));
                            le_device_db_remove(i);
                            break;
                        }
                    }
                }
                sm_request_pairing(h);
            }
            break;
        }

        default:
            break;
    }

    if (connect_hids_handle != HCI_CON_HANDLE_INVALID) {
        BleSlot *slot = find_slot_by_handle(connect_hids_handle);
        if (slot) {
            printf("[BLE Host] Connecting HIDS client for '%s' (slot %u, handle 0x%04X, mode %u)...\n",
                   slot->name, slot->dev_idx, slot->con_handle, s_protocol_mode);
            uint8_t status = hids_client_connect(slot->con_handle, &gattPacketHandler, s_protocol_mode, &slot->hids_cid);
            if (status != ERROR_CODE_SUCCESS) {
                printf("[BLE Host] hids_client_connect failed with status 0x%02X\n", status);
            }
        }
    }
}

void BleHidHost::packetHandler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;

    uint8_t event = hci_event_packet_get_type(packet);
    if (event == HCI_EVENT_LE_META) {
        // LE meta events carry the advertising reports of the background scan (several per second);
        // everything else (connection complete, parameter updates...) is worth a breadcrumb.
        uint8_t sub = hci_event_le_meta_get_subevent_code(packet);
        if (sub != HCI_SUBEVENT_LE_ADVERTISING_REPORT && sub != HCI_SUBEVENT_LE_EXTENDED_ADVERTISING_REPORT) {
            LogRing::breadcrumb(CRUMB_HCI(0x100 | sub));
        }
    } else if (event != GAP_EVENT_ADVERTISING_REPORT && event != HCI_EVENT_NUMBER_OF_COMPLETED_PACKETS) {
        LogRing::breadcrumb(CRUMB_HCI(event));  // advertising reports and ACL credits are too frequent
    }
    switch (event) {
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
                printf("[BLE Host] BTstack HCI State: WORKING\n");
                tryAutoReconnectOrScan();
            }
            break;

        case GAP_EVENT_ADVERTISING_REPORT: {
            if (!s_is_scanning) break;
            if (s_is_connecting || s_eviction_pending) break;

            bd_addr_t addr;
            gap_event_advertising_report_get_address(packet, addr);
            if (find_slot_by_addr(addr) != nullptr) break;

            uint8_t addr_type = gap_event_advertising_report_get_address_type(packet);
            const uint8_t *ad_data = gap_event_advertising_report_get_data(packet);
            uint8_t ad_len = gap_event_advertising_report_get_data_length(packet);
            int8_t rssi = (int8_t)gap_event_advertising_report_get_rssi(packet);

            AdvDeviceInfo info;
            parse_adv_data(ad_data, ad_len, &info);

            uint32_t now = to_ms_since_boot(get_absolute_time());
            bool should_log = update_and_should_log_device(addr, now, &info);
            bool is_bonded = is_bonded_device_addr(addr);
            bool is_target = is_target_hid_device(info);
            // In pairing mode, allow new unbonded target HID devices.
            // In regular background scanning, strictly reconnect to already-bonded devices.
            bool is_candidate = is_bonded || (s_is_pairing_mode && is_target);
            if (should_log && (is_candidate || (s_stack_logging && info.name[0] != '\0'))) {
                if (info.name[0] != '\0') {
                    printf("[BLE Host] Adv: '%s' (%s, RSSI %d dBm, HID=%d, App=0x%04X)%s\n",
                           info.name, bd_addr_to_str(addr), rssi, info.has_hid_service, info.appearance,
                           is_bonded ? " [bonded]" : (s_is_pairing_mode ? " [pair-candidate]" : ""));
                } else {
                    printf("[BLE Host] Adv: %s (RSSI %d dBm, HID=%d, App=0x%04X)%s\n",
                           bd_addr_to_str(addr), rssi, info.has_hid_service, info.appearance,
                           is_bonded ? " [bonded]" : (s_is_pairing_mode ? " [pair-candidate]" : ""));
                }
            }
            if (is_candidate) {
                const char *dev_name = (info.name[0] != '\0') ? info.name : "";
                if (dev_name[0] == '\0' && is_bonded) {
                    for (uint8_t b = 0; b < s_bonded_table.count; b++) {
                        if (bd_addr_cmp(addr, s_bonded_table.records[b].addr) == 0) {
                            dev_name = s_bonded_table.records[b].name;
                            break;
                        }
                    }
                }

                printf("[BLE Host] %s found: '%s' (%s)\n",
                       is_bonded ? "Bonded device" : "Pairing candidate",
                       dev_name[0] != '\0' ? dev_name : bd_addr_to_str(addr),
                       bd_addr_to_str(addr));
                connectToDevice(addr, (bd_addr_type_t)addr_type, dev_name);
            }
            break;
        }

        case HCI_EVENT_DISCONNECTION_COMPLETE: {
            hci_con_handle_t handle = hci_event_disconnection_complete_get_connection_handle(packet);
            uint8_t reason = hci_event_disconnection_complete_get_reason(packet);
            BleSlot *slot = find_slot_by_handle(handle);
            printf("[BLE Host] Device disconnected on handle 0x%04X (reason 0x%02X, slot %u).\n",
                   handle, reason, slot ? slot->dev_idx : 0xFF);

            if (slot) {
                Multiplexer::purgeKeyboard(slot->dev_idx);
                Multiplexer::purgeMouse(slot->dev_idx);
                reset_slot(slot);
            }

            if (s_pending_pairing_handle == handle) {
                s_pending_pairing_handle = HCI_CON_HANDLE_INVALID;
                btstack_run_loop_remove_timer(&s_pairing_timer);
            }

            if (s_eviction_pending) {
                s_eviction_pending = false;
                printf("[BLE Host] Slot freed after LRU eviction. Proceeding to connect '%s'...\n",
                       s_pending_connect_name);
                connectToDevice(s_pending_connect_addr, s_pending_connect_addr_type, s_pending_connect_name);
            } else {
                tryAutoReconnectOrScan();
            }
            break;
        }

        case HCI_EVENT_META_GAP: {
            uint8_t subevent = hci_event_gap_meta_get_subevent_code(packet);
            if (subevent == GAP_SUBEVENT_LE_CONNECTION_COMPLETE) {
                btstack_run_loop_remove_timer(&s_reconnect_timer);
                uint8_t status = gap_subevent_le_connection_complete_get_status(packet);
                hci_con_handle_t handle = gap_subevent_le_connection_complete_get_connection_handle(packet);

                if (status != ERROR_CODE_SUCCESS) {
                    printf("[BLE Host] LE Connection failed (status 0x%02X). Resuming scan...\n", status);
                    if (s_connecting_slot_idx >= 0 && s_connecting_slot_idx < MAX_BLE_DEVICES) {
                        reset_slot(&s_slots[s_connecting_slot_idx]);
                    }
                    s_is_connecting = false;
                    s_connecting_slot_idx = -1;
                    s_is_scanning = false;
                    s_eviction_pending = false;
                    tryAutoReconnectOrScan();
                    break;
                }

                BleSlot *slot = nullptr;
                if (s_connecting_slot_idx >= 0 && s_connecting_slot_idx < MAX_BLE_DEVICES) {
                    slot = &s_slots[s_connecting_slot_idx];
                } else {
                    bd_addr_t peer_addr;
                    gap_subevent_le_connection_complete_get_peer_address(packet, peer_addr);
                    slot = find_slot_by_addr(peer_addr);
                }

                if (!slot) {
                    slot = find_free_slot();
                }

                s_is_connecting = false;
                s_connecting_slot_idx = -1;

                if (slot) {
                    slot->last_used_ms = to_ms_since_boot(get_absolute_time());
                    slot->con_handle = handle;
                    uint16_t interval = gap_subevent_le_connection_complete_get_conn_interval(packet);
                    uint16_t latency = gap_subevent_le_connection_complete_get_conn_latency(packet);
                    slot->conn_interval = interval;
                    slot->conn_latency = latency;
                    if (interval > 0) {
                        slot->min_conn_interval = interval;
                    }
                    printf("[BLE Host] LE Connection established for '%s' (slot %u, handle 0x%04X, interval %.2f ms, latency %u). Scheduling security in 200ms...\n",
                           slot->name, slot->dev_idx, handle, interval * 1.25f, latency);
                    s_pending_pairing_handle = handle;
                    btstack_run_loop_remove_timer(&s_pairing_timer);
                    btstack_run_loop_set_timer(&s_pairing_timer, 200);
                    btstack_run_loop_set_timer_handler(&s_pairing_timer, &onPairingDelayTimeout);
                    btstack_run_loop_add_timer(&s_pairing_timer);
                }
            }
            break;
        }

        case HCI_EVENT_ENCRYPTION_CHANGE:
        case HCI_EVENT_ENCRYPTION_CHANGE_V2: {
            hci_con_handle_t handle = hci_event_encryption_change_get_connection_handle(packet);
            uint8_t status = hci_event_encryption_change_get_status(packet);
            uint8_t enabled = hci_event_encryption_change_get_encryption_enabled(packet);
            BleSlot *slot = find_slot_by_handle(handle);
            printf("[BLE Host] Encryption change on handle 0x%04X (slot %u): status 0x%02X, enabled %u\n",
                   handle, slot ? slot->dev_idx : 0xFF, status, enabled);
            break;
        }

        case HCI_EVENT_LE_META: {
            uint8_t subevent = hci_event_le_meta_get_subevent_code(packet);
            if (subevent == HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE) {
                uint16_t interval = hci_subevent_le_connection_update_complete_get_conn_interval(packet);
                uint16_t latency = hci_subevent_le_connection_update_complete_get_conn_latency(packet);
                uint16_t timeout = hci_subevent_le_connection_update_complete_get_supervision_timeout(packet);
                hci_con_handle_t handle = hci_subevent_le_connection_update_complete_get_connection_handle(packet);
                BleSlot *slot = find_slot_by_handle(handle);
                if (slot) {
                    slot->conn_interval = interval;
                    slot->conn_latency = latency;
                    if (slot->min_conn_interval == 0 || (interval > 0 && interval < slot->min_conn_interval)) {
                        slot->min_conn_interval = interval;
                    }
                }
                printf("[BLE Host] Connection parameters updated (slot %u): interval %.2f ms (min %.2f ms), latency %u, timeout %u ms\n",
                       slot ? slot->dev_idx : 0xFF, interval * 1.25f,
                       slot ? slot->min_conn_interval * 1.25f : 0.0f,
                       latency, timeout * 10);
#if BLE_ZERO_SLAVE_LATENCY
                // Keep the peripheral's shortest observed interval and zero its slave latency
                // once it has stopped changing parameters itself.
                if (slot && (latency > 0 || (slot->min_conn_interval > 0 && interval > slot->min_conn_interval))) {
                    scheduleZeroLatency(slot);
                }
#endif
            }
            break;
        }

        default:
            break;
    }
}
