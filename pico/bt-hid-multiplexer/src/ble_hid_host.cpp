#include "ble_hid_host.h"
#include "multiplexer.h"
#include "config.h"
#include "btstack.h"
#include "btstack_tlv.h"
#include "ble/gatt-service/hids_client.h"
#include "ble/le_device_db.h"
#include "pico/time.h"
#include <stdio.h>
#include <string.h>

// TAG to store remote device address, type, and name in TLV
#define TLV_TAG_HOGD ((((uint32_t) 'H') << 24 ) | (((uint32_t) 'O') << 16) | (((uint32_t) 'G') << 8) | 'D')

struct BondedDeviceRecord {
    bd_addr_t addr;
    bd_addr_type_t addr_type;
    char name[32];
};

static bool s_is_scanning = false;
static bool s_is_connecting = false;
static bool s_connected = false;
static uint32_t s_active_passkey = 0;
static char s_connected_dev_name[32] = {0};

// Cached bonded peripheral info in RAM to avoid flash reads in packet handlers
static bool s_has_bonded_device = false;
static BondedDeviceRecord s_bonded_device;

static bd_addr_t s_remote_addr;
static bd_addr_type_t s_remote_addr_type = BD_ADDR_TYPE_LE_PUBLIC;
static hci_con_handle_t s_con_handle = HCI_CON_HANDLE_INVALID;
static uint16_t s_hids_cid = 0;
static hid_protocol_mode_t s_protocol_mode = HID_PROTOCOL_MODE_REPORT;

// Timer and handler for targeted auto-reconnect fallback
static btstack_timer_source_t s_reconnect_timer;
static void onReconnectTimeout(btstack_timer_source_t *ts);
static void tryAutoReconnectOrScan();
static void connectToDevice(const bd_addr_t addr, bd_addr_type_t addr_type, const char *name);

// Timer to allow Link Layer connection anchors to stabilize before sending pairing/security requests
static btstack_timer_source_t s_pairing_timer;
static void onPairingDelayTimeout(btstack_timer_source_t *ts) {
    (void)ts;
    if (s_con_handle != HCI_CON_HANDLE_INVALID && !s_connected) {
        printf("[BLE Host] Link Layer established. Requesting security/pairing...\n");
        sm_request_pairing(s_con_handle);
    }
}

// Report descriptor buffer sized to accommodate complex composite peripherals (keyboard + touchpad)
static uint8_t s_hid_descriptor_storage[2048];
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

struct AdvDeviceInfo {
    bool has_hid_service;
    uint16_t appearance;
    char name[32];
};

// Case-insensitive match for common keyboard, mouse, and trackpad vendor names
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
            strstr(lower, "trackpad")  != NULL ||
            strstr(lower, "touchpad")  != NULL ||
            strstr(lower, "trackball") != NULL ||
            strstr(lower, "nape")      != NULL ||
            strstr(lower, "mouse")     != NULL ||
            strstr(lower, "xk01")     != NULL ||
            strstr(lower, "k380")     != NULL ||
            strstr(lower, "k480")     != NULL ||
            strstr(lower, "logi")     != NULL);
}

// Parses BLE advertisement or scan response data for HID service, appearance, and name
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

// Evaluates whether discovered peripheral matches HID device characteristics
static bool is_target_hid_device(const AdvDeviceInfo &info) {
    // 1. Matches BLE HID Service UUID (0x1812)
    if (info.has_hid_service) return true;

    // 2. Matches BLE Appearance for HID category (0x03C0 - 0x03CF, e.g. Keyboard 0x03C1, Mouse 0x03C2, Touchpad 0x03C9)
    if (info.appearance >= 0x03C0 && info.appearance <= 0x03CF) return true;

    // 3. Matches known keyboard/mouse/trackpad names
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

// Updates seen device records across advertisement and scan response packets,
// and throttles discovery log messages per device address
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


// Load bonded device record from TLV storage
static bool load_bonded_device(BondedDeviceRecord *record) {
    const btstack_tlv_t *tlv_impl = nullptr;
    void *tlv_context = nullptr;
    btstack_tlv_get_instance(&tlv_impl, &tlv_context);
    if (!tlv_impl) return false;

    int len = tlv_impl->get_tag(tlv_context, TLV_TAG_HOGD, (uint8_t *)record, sizeof(BondedDeviceRecord));
    return (len == (int)sizeof(BondedDeviceRecord));
}

// Persist bonded device record in TLV storage
static void save_bonded_device(const bd_addr_t addr, bd_addr_type_t addr_type, const char *name) {
    const btstack_tlv_t *tlv_impl = nullptr;
    void *tlv_context = nullptr;
    btstack_tlv_get_instance(&tlv_impl, &tlv_context);
    if (!tlv_impl) return;

    BondedDeviceRecord record;
    bd_addr_copy(record.addr, addr);
    record.addr_type = addr_type;
    if (name && name[0] != '\0') {
        strncpy(record.name, name, sizeof(record.name) - 1);
        record.name[sizeof(record.name) - 1] = '\0';
    } else {
        snprintf(record.name, sizeof(record.name), "%s", bd_addr_to_str(addr));
    }

    tlv_impl->store_tag(tlv_context, TLV_TAG_HOGD, (const uint8_t *)&record, sizeof(record));
    printf("[BLE Host] Saved bonded device to TLV: '%s' (%s, type %u)\n",
           record.name, bd_addr_to_str(record.addr), record.addr_type);
}

// Remove bonded device record from TLV storage
static void delete_bonded_device() {
    const btstack_tlv_t *tlv_impl = nullptr;
    void *tlv_context = nullptr;
    btstack_tlv_get_instance(&tlv_impl, &tlv_context);
    if (tlv_impl) {
        tlv_impl->delete_tag(tlv_context, TLV_TAG_HOGD);
        printf("[BLE Host] Removed bonded device record from TLV.\n");
    }
}

// Check whether a device address matches a known bonded device in RAM cache or le_device_db
static bool is_bonded_device_addr(const bd_addr_t addr) {
    if (s_has_bonded_device && bd_addr_cmp(addr, s_bonded_device.addr) == 0) {
        return true;
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
        printf("[BLE Host] Connection attempt timed out. Resuming discovery scan...\n");
        s_is_connecting = false;
        s_connected_dev_name[0] = '\0';
        gap_connect_cancel();
        BleHidHost::startScan();
    }
}

// Centralized connection routine with 10-second timeout
static void connectToDevice(const bd_addr_t addr, bd_addr_type_t addr_type, const char *name) {
    if (s_is_connecting || s_connected) return;

    btstack_run_loop_remove_timer(&s_reconnect_timer);
    gap_stop_scan();
    s_is_scanning = false;
    s_is_connecting = true;

    bd_addr_copy(s_remote_addr, addr);
    s_remote_addr_type = addr_type;
    if (name && name[0] != '\0') {
        strncpy(s_connected_dev_name, name, sizeof(s_connected_dev_name) - 1);
        s_connected_dev_name[sizeof(s_connected_dev_name) - 1] = '\0';
    } else {
        snprintf(s_connected_dev_name, sizeof(s_connected_dev_name), "%s", bd_addr_to_str(addr));
    }

    printf("[BLE Host] Connecting to '%s' (%s, type %u)...\n",
           s_connected_dev_name, bd_addr_to_str(s_remote_addr), (unsigned)s_remote_addr_type);

    // Set 10-second timeout for outgoing connection
    btstack_run_loop_set_timer(&s_reconnect_timer, 10000);
    btstack_run_loop_set_timer_handler(&s_reconnect_timer, &onReconnectTimeout);
    btstack_run_loop_add_timer(&s_reconnect_timer);

    gap_connect(s_remote_addr, s_remote_addr_type);
}

// Start continuous scanning for bonded peripheral reconnection or new HID devices
static void tryAutoReconnectOrScan() {
    btstack_run_loop_remove_timer(&s_reconnect_timer);

    if (s_has_bonded_device) {
        printf("[BLE Host] Bonded peripheral stored: '%s' (%s). Scanning for reconnection...\n",
               s_bonded_device.name, bd_addr_to_str(s_bonded_device.addr));
    } else {
        printf("[BLE Host] No bonded device stored. Starting discovery scan...\n");
    }
    BleHidHost::startScan();
}

static void load_bonded_device_cache() {
    s_has_bonded_device = load_bonded_device(&s_bonded_device);
    if (!s_has_bonded_device) {
        // Fall back to le_device_db if TLV tag not yet written
        for (int i = 0; i < le_device_db_max_count(); i++) {
            int addr_type = 0;
            bd_addr_t addr;
            le_device_db_info(i, &addr_type, addr, nullptr);
            if (addr_type != BD_ADDR_TYPE_UNKNOWN) {
                s_has_bonded_device = true;
                bd_addr_copy(s_bonded_device.addr, addr);
                s_bonded_device.addr_type = (bd_addr_type_t)addr_type;
                snprintf(s_bonded_device.name, sizeof(s_bonded_device.name), "%s", bd_addr_to_str(addr));
                break;
            }
        }
    }
    if (s_has_bonded_device) {
        printf("[BLE Host] Loaded bonded device: '%s' (%s, type %u)\n",
               s_bonded_device.name, bd_addr_to_str(s_bonded_device.addr), (unsigned)s_bonded_device.addr_type);
    } else {
        printf("[BLE Host] No bonded device stored.\n");
    }
}

void BleHidHost::init() {
    l2cap_init();

    // Security Manager setup: Display Only for 6-digit keyboard passkey pairing
    sm_init();
    sm_set_io_capabilities(IO_CAPABILITY_DISPLAY_ONLY);
    sm_set_authentication_requirements(SM_AUTHREQ_BONDING | SM_AUTHREQ_SECURE_CONNECTION);

    gatt_client_init();
    att_server_init(s_profile_data, NULL, NULL);

    hids_client_init(s_hid_descriptor_storage, sizeof(s_hid_descriptor_storage));

    s_hci_event_callback_registration.callback = &packetHandler;
    hci_add_event_handler(&s_hci_event_callback_registration);

    s_sm_event_callback_registration.callback = &smPacketHandler;
    sm_add_event_handler(&s_sm_event_callback_registration);

    // Active continuous scanning (type 1) to fetch Scan Response packets containing HID UUIDs and names
    gap_set_scan_parameters(1, 48, 48);

    // Initial connection parameters: 30ms scan window/interval, 15-30ms conn interval, 4s supervision timeout
    gap_set_connection_parameters(48, 48, 12, 24, 0, 400, 0, 0);

    s_is_scanning = false;
    s_is_connecting = false;
    s_connected = false;
    s_active_passkey = 0;
    s_con_handle = HCI_CON_HANDLE_INVALID;
    s_connected_dev_name[0] = '\0';

    load_bonded_device_cache();

    hci_power_control(HCI_POWER_ON);
}

void BleHidHost::startScan() {
    btstack_run_loop_remove_timer(&s_reconnect_timer);
    btstack_run_loop_remove_timer(&s_pairing_timer);
    if (s_is_connecting) {
        s_is_connecting = false;
        gap_connect_cancel();
    }
    if (s_is_scanning) return;
    printf("[BLE Host] Starting scan for BLE HID peripherals...\n");
    s_is_scanning = true;
    s_active_passkey = 0;
    // Operate in Report Protocol Mode to ensure report characteristic discovery and CCCD notification subscription
    s_protocol_mode = HID_PROTOCOL_MODE_REPORT;
    memset(s_seen_devices, 0, sizeof(s_seen_devices));
    gap_start_scan();
}

void BleHidHost::stopScan() {
    btstack_run_loop_remove_timer(&s_reconnect_timer);
    btstack_run_loop_remove_timer(&s_pairing_timer);
    if (!s_is_scanning) return;
    printf("[BLE Host] Stopping BLE scan.\n");
    s_is_scanning = false;
    gap_stop_scan();
}

bool BleHidHost::isScanning() {
    return s_is_scanning;
}

bool BleHidHost::isConnected() {
    return s_connected;
}

const char* BleHidHost::getConnectedDeviceName() {
    return (s_connected && s_connected_dev_name[0] != '\0') ? s_connected_dev_name : "None";
}

uint32_t BleHidHost::getActivePasskey() {
    return s_active_passkey;
}

void BleHidHost::clearPasskey() {
    s_active_passkey = 0;
}

void BleHidHost::clearBonds() {
    printf("[BLE Host] Clearing all bonded devices.\n");
    btstack_run_loop_remove_timer(&s_pairing_timer);
    s_has_bonded_device = false;
    memset(&s_bonded_device, 0, sizeof(s_bonded_device));
    s_connected_dev_name[0] = '\0';
    delete_bonded_device();
    for (int i = 0; i < le_device_db_max_count(); i++) {
        le_device_db_remove(i);
    }
    if (s_con_handle != HCI_CON_HANDLE_INVALID) {
        gap_disconnect(s_con_handle);
    }
}

void BleHidHost::dumpBonds() {
    printf("[BLE Host] Bonded cache: %s\n", s_has_bonded_device ? s_bonded_device.name : "None");
    if (s_has_bonded_device) {
        printf("  Address: %s (type %u)\n", bd_addr_to_str(s_bonded_device.addr), (unsigned)s_bonded_device.addr_type);
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
        printf("  (no bonded devices in le_device_db)\n");
    }
}

void BleHidHost::sendHostLeds(uint8_t leds) {
    if (s_connected && s_hids_cid != 0) {
        // Send Output Report containing keyboard LED status
        uint8_t led_report = leds;
        hids_client_send_write_report(s_hids_cid, 0, HID_REPORT_TYPE_OUTPUT, &led_report, 1);
    }
}

void BleHidHost::dumpDescriptor() {
    if (!s_connected || s_hids_cid == 0) {
        printf("[BLE Host] No active HIDS connection to dump descriptor.\n");
        return;
    }
    const uint8_t *desc = hids_client_descriptor_storage_get_descriptor_data(s_hids_cid, 0);
    uint16_t desc_len = hids_client_descriptor_storage_get_descriptor_len(s_hids_cid, 0);
    printf("[BLE Host] Stored HID Report Descriptor (len %u):\n", desc_len);
    if (desc && desc_len > 0) {
        printf_hexdump(desc, desc_len);
    } else {
        printf("[BLE Host] No HID descriptor available in storage\n");
    }
}

void BleHidHost::gattPacketHandler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET && packet_type != HCI_EVENT_GATTSERVICE_META) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_GATTSERVICE_META) return;

    uint8_t subevent = hci_event_gattservice_meta_get_subevent_code(packet);
    switch (subevent) {
        case GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED: {
            uint8_t status = gattservice_subevent_hid_service_connected_get_status(packet);
            if (status == ERROR_CODE_SUCCESS) {
                uint8_t mode = gattservice_subevent_hid_service_connected_get_protocol_mode(packet);
                uint8_t instances = gattservice_subevent_hid_service_connected_get_num_instances(packet);
                printf("[BLE Host] HID Service Connected successfully! (mode: %u, instances: %u)\n", mode, instances);
                s_connected = true;
                s_active_passkey = 0;

                // Update bonded device in RAM cache and persist to TLV
                s_has_bonded_device = true;
                bd_addr_copy(s_bonded_device.addr, s_remote_addr);
                s_bonded_device.addr_type = s_remote_addr_type;
                strncpy(s_bonded_device.name, s_connected_dev_name, sizeof(s_bonded_device.name) - 1);
                s_bonded_device.name[sizeof(s_bonded_device.name) - 1] = '\0';
                save_bonded_device(s_remote_addr, s_remote_addr_type, s_connected_dev_name);

                // Dump raw HID Report Descriptor to inspect report IDs and features
                const uint8_t *desc = hids_client_descriptor_storage_get_descriptor_data(s_hids_cid, 0);
                uint16_t desc_len = hids_client_descriptor_storage_get_descriptor_len(s_hids_cid, 0);
                printf("[BLE Host] HID Report Descriptor (len %u):\n", desc_len);
                if (desc && desc_len > 0) {
                    printf_hexdump(desc, desc_len);
                } else {
                    printf("[BLE Host] No HID descriptor available\n");
                }

                // Explicitly ensure report notifications are enabled across all discovered input reports
                hids_client_enable_notifications(s_hids_cid);

                // Request lowest latency BLE connection interval (6 * 1.25ms = 7.5ms = 133.3 Hz)
                printf("[BLE Host] Requesting low-latency connection interval (7.5ms - 10ms)...\n");
                gap_update_connection_parameters(s_con_handle, 6, 8, 0, 400);
            } else {
                printf("[BLE Host] HID Service connection failed, status: 0x%02X\n", status);
                if (s_protocol_mode == HID_PROTOCOL_MODE_BOOT && s_con_handle != HCI_CON_HANDLE_INVALID) {
                    printf("[BLE Host] Retrying HIDS connection in Report Mode...\n");
                    s_protocol_mode = HID_PROTOCOL_MODE_REPORT;
                    hids_client_connect(s_con_handle, &gattPacketHandler, s_protocol_mode, &s_hids_cid);
                    return;
                }
                s_connected = false;
                if (s_con_handle != HCI_CON_HANDLE_INVALID) {
                    gap_disconnect(s_con_handle);
                }
            }
            break;
        }

        case GATTSERVICE_SUBEVENT_HID_SERVICE_REPORTS_NOTIFICATION:
            printf("[BLE Host] HID Reports notification configuration active.\n");
            break;

        case GATTSERVICE_SUBEVENT_HID_SERVICE_DISCONNECTED:
            printf("[BLE Host] HID Service Disconnected.\n");
            s_connected = false;
            Multiplexer::purgeKeyboard(0);
            Multiplexer::purgeMouse(0);
            break;

        case GATTSERVICE_SUBEVENT_HID_REPORT: {
            const uint8_t *report = gattservice_subevent_hid_report_get_report(packet);
            uint16_t len = gattservice_subevent_hid_report_get_report_len(packet);
            uint8_t report_id = gattservice_subevent_hid_report_get_report_id(packet);
            if (!report || len == 0) break;

            // BTstack prepends report_id at index 0 when delivering report notifications.
            // When report_id matches, payload begins at index 1 with length len - 1.
            const uint8_t *data = report;
            uint16_t data_len = len;
            if (len > 1 && report[0] == report_id) {
                data = &report[1];
                data_len = len - 1;
            }

            // Route incoming reports by length and report ID:
            // 8-byte reports: Standard keyboard [modifier, reserved, k0..k5]
            // 3-5 byte reports: Standard 8-bit mouse/trackpad [buttons, dx, dy, optional wheel, optional pan]
            // 6-byte reports: 16-bit mouse displacement with wheel [buttons, dx_l, dx_h, dy_l, dy_h, wheel]
            // 7-byte reports: ProtoArc 16-bit relative trackpad on report_id 2, or 7-byte keyboard [modifier, k0..k5]
            if (data_len == 8) {
                // Standard Keyboard report with modifier, reserved byte, and 6 keycodes
                Multiplexer::handleKeyboardReport(0, data[0], &data[2], 6);
            } else if (data_len >= 3 && data_len <= 5) {
                // Standard 8-bit displacement: [buttons, dx, dy, optional wheel, optional pan]
                uint8_t buttons = data[0];
                int16_t dx = (int8_t)data[1];
                int16_t dy = (int8_t)data[2];
                int8_t wheel = (data_len >= 4) ? (int8_t)data[3] : 0;
                int8_t pan   = (data_len >= 5) ? (int8_t)data[4] : 0;
                Multiplexer::handleMouseReport(0, buttons, dx, dy, wheel, pan);
            } else if (data_len == 6) {
                // 16-bit displacement with vertical wheel: [buttons, dx_l, dx_h, dy_l, dy_h, wheel]
                uint8_t buttons = data[0];
                int16_t dx = (int16_t)((uint16_t)data[1] | ((uint16_t)data[2] << 8));
                int16_t dy = (int16_t)((uint16_t)data[3] | ((uint16_t)data[4] << 8));
                int8_t wheel = (int8_t)data[5];
                Multiplexer::handleMouseReport(0, buttons, dx, dy, wheel, 0);
            } else if (data_len == 7) {
                if (report_id == 2) {
                    // ProtoArc 16-bit relative trackpad report: [buttons, dx_l, dx_h, dy_l, dy_h, wheel, pan]
                    uint8_t buttons = data[0];
                    int16_t dx = (int16_t)((uint16_t)data[1] | ((uint16_t)data[2] << 8));
                    int16_t dy = (int16_t)((uint16_t)data[3] | ((uint16_t)data[4] << 8));
                    int8_t wheel = (int8_t)data[5];
                    int8_t pan = (int8_t)data[6];
                    Multiplexer::handleMouseReport(0, buttons, dx, dy, wheel, pan);
                } else {
                    // Keyboard report with modifier and 6 keycodes (omitting reserved byte)
                    Multiplexer::handleKeyboardReport(0, data[0], &data[1], 6);
                }
            } else if (data_len == 9) {
                // Keychron Nape Pro 16-bit mouse/trackball with 16-bit wheel and pan:
                // [buttons, dx_l, dx_h, dy_l, dy_h, wheel_l, wheel_h, pan_l, pan_h]
                uint8_t buttons = data[0];
                int16_t dx    = (int16_t)((uint16_t)data[1] | ((uint16_t)data[2] << 8));
                int16_t dy    = (int16_t)((uint16_t)data[3] | ((uint16_t)data[4] << 8));
                int16_t wheel = (int16_t)((uint16_t)data[5] | ((uint16_t)data[6] << 8));
                int16_t pan   = (int16_t)((uint16_t)data[7] | ((uint16_t)data[8] << 8));
                Multiplexer::handleMouseReport(0, buttons, dx, dy, (int8_t)wheel, (int8_t)pan);
            } else {
                printf("[BLE Host] Unhandled HID Report (id %u, data_len %u)\n", report_id, data_len);
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
    printf("[BLE Host] SM Event: 0x%02X\n", sm_event);

    bool connect_hids = false;

    switch (sm_event) {
        case SM_EVENT_JUST_WORKS_REQUEST:
            printf("[BLE Host] Just Works pairing requested. Confirming...\n");
            sm_just_works_confirm(sm_event_just_works_request_get_handle(packet));
            break;

        case SM_EVENT_NUMERIC_COMPARISON_REQUEST:
            printf("[BLE Host] Numeric comparison confirmed.\n");
            sm_numeric_comparison_confirm(sm_event_numeric_comparison_request_get_handle(packet));
            break;

        case SM_EVENT_PASSKEY_DISPLAY_NUMBER:
            s_active_passkey = sm_event_passkey_display_number_get_passkey(packet);
            printf("[BLE Host] >>> PAIRING PASSKEY: %06lu <<<\n", (unsigned long)s_active_passkey);
            break;

        case SM_EVENT_PAIRING_COMPLETE: {
            uint8_t status = sm_event_pairing_complete_get_status(packet);
            uint8_t reason = sm_event_pairing_complete_get_reason(packet);
            if (status == ERROR_CODE_SUCCESS) {
                printf("[BLE Host] Pairing complete: SUCCESS\n");
                s_active_passkey = 0;
                connect_hids = true;

                // Cache and persist bonded device immediately on pairing completion
                s_has_bonded_device = true;
                bd_addr_copy(s_bonded_device.addr, s_remote_addr);
                s_bonded_device.addr_type = s_remote_addr_type;
                strncpy(s_bonded_device.name, s_connected_dev_name, sizeof(s_bonded_device.name) - 1);
                s_bonded_device.name[sizeof(s_bonded_device.name) - 1] = '\0';
                save_bonded_device(s_remote_addr, s_remote_addr_type, s_connected_dev_name);
            } else {
                printf("[BLE Host] Pairing complete: FAILED (status 0x%02X, reason 0x%02X)\n", status, reason);
                s_active_passkey = 0;
                if (s_con_handle != HCI_CON_HANDLE_INVALID) {
                    gap_disconnect(s_con_handle);
                }
            }
            break;
        }

        case SM_EVENT_REENCRYPTION_COMPLETE: {
            uint8_t status = sm_event_reencryption_complete_get_status(packet);
            if (status == ERROR_CODE_SUCCESS) {
                printf("[BLE Host] Re-encryption complete: SUCCESS\n");
                connect_hids = true;
            } else {
                printf("[BLE Host] Re-encryption failed (status 0x%02X). Requesting fresh pairing...\n", status);
                sm_request_pairing(s_con_handle);
            }
            break;
        }

        default:
            break;
    }

    if (connect_hids && s_con_handle != HCI_CON_HANDLE_INVALID) {
        printf("[BLE Host] Connecting HIDS client (mode %u)...\n", s_protocol_mode);
        uint8_t status = hids_client_connect(s_con_handle, &gattPacketHandler, s_protocol_mode, &s_hids_cid);
        if (status != ERROR_CODE_SUCCESS) {
            printf("[BLE Host] hids_client_connect failed with status 0x%02X\n", status);
        }
    }
}

void BleHidHost::packetHandler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;

    uint8_t event = hci_event_packet_get_type(packet);
    switch (event) {
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
                printf("[BLE Host] BTstack HCI State: WORKING\n");
                tryAutoReconnectOrScan();
            }
            break;

        case GAP_EVENT_ADVERTISING_REPORT: {
            if (!s_is_scanning) break;

            bd_addr_t addr;
            gap_event_advertising_report_get_address(packet, addr);
            uint8_t addr_type = gap_event_advertising_report_get_address_type(packet);
            const uint8_t *ad_data = gap_event_advertising_report_get_data(packet);
            uint8_t ad_len = gap_event_advertising_report_get_data_length(packet);
            int8_t rssi = (int8_t)gap_event_advertising_report_get_rssi(packet);

            AdvDeviceInfo info;
            parse_adv_data(ad_data, ad_len, &info);

            uint32_t now = to_ms_since_boot(get_absolute_time());
            bool should_log = update_and_should_log_device(addr, now, &info);
            if (should_log) {
                if (info.name[0] != '\0') {
                    printf("[BLE Host] Adv: '%s' (%s, RSSI %d dBm, HID=%d, App=0x%04X)\n",
                           info.name, bd_addr_to_str(addr), rssi, info.has_hid_service, info.appearance);
                } else {
                    printf("[BLE Host] Adv: %s (RSSI %d dBm, HID=%d, App=0x%04X)\n",
                           bd_addr_to_str(addr), rssi, info.has_hid_service, info.appearance);
                }
            }

            bool is_bonded = is_bonded_device_addr(addr);
            if (is_bonded || is_target_hid_device(info)) {
                const char *dev_name = (info.name[0] != '\0') ? info.name :
                                       (is_bonded ? s_bonded_device.name : "");
                printf("[BLE Host] %s found: '%s' (%s)\n",
                       is_bonded ? "Bonded device" : "Target HID device",
                       dev_name[0] != '\0' ? dev_name : bd_addr_to_str(addr),
                       bd_addr_to_str(addr));
                connectToDevice(addr, (bd_addr_type_t)addr_type, dev_name);
            }
            break;
        }

        case HCI_EVENT_DISCONNECTION_COMPLETE: {
            uint8_t reason = hci_event_disconnection_complete_get_reason(packet);
            printf("[BLE Host] Device disconnected (reason 0x%02X). Resuming scan/reconnect...\n", reason);
            btstack_run_loop_remove_timer(&s_reconnect_timer);
            btstack_run_loop_remove_timer(&s_pairing_timer);
            s_con_handle = HCI_CON_HANDLE_INVALID;
            s_connected = false;
            s_is_connecting = false;
            s_active_passkey = 0;
            s_connected_dev_name[0] = '\0';
            Multiplexer::purgeKeyboard(0);
            Multiplexer::purgeMouse(0);
            tryAutoReconnectOrScan();
            break;
        }

        case HCI_EVENT_META_GAP: {
            uint8_t subevent = hci_event_gap_meta_get_subevent_code(packet);
            if (subevent == GAP_SUBEVENT_LE_CONNECTION_COMPLETE) {
                btstack_run_loop_remove_timer(&s_reconnect_timer);
                s_is_connecting = false;
                uint8_t status = gap_subevent_le_connection_complete_get_status(packet);
                if (status != ERROR_CODE_SUCCESS) {
                    printf("[BLE Host] LE Connection failed (status 0x%02X). Resuming scan...\n", status);
                    btstack_run_loop_remove_timer(&s_pairing_timer);
                    s_con_handle = HCI_CON_HANDLE_INVALID;
                    s_connected = false;
                    s_active_passkey = 0;
                    startScan();
                    break;
                }
                s_con_handle = gap_subevent_le_connection_complete_get_connection_handle(packet);
                printf("[BLE Host] LE Connection established (handle 0x%04X). Scheduling security/pairing in 200ms...\n", s_con_handle);
                btstack_run_loop_remove_timer(&s_pairing_timer);
                btstack_run_loop_set_timer(&s_pairing_timer, 200);
                btstack_run_loop_set_timer_handler(&s_pairing_timer, &onPairingDelayTimeout);
                btstack_run_loop_add_timer(&s_pairing_timer);
            } else if (subevent == HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE) {
                uint16_t interval = hci_subevent_le_connection_update_complete_get_conn_interval(packet);
                uint16_t latency = hci_subevent_le_connection_update_complete_get_conn_latency(packet);
                uint16_t timeout = hci_subevent_le_connection_update_complete_get_supervision_timeout(packet);
                printf("[BLE Host] Connection parameters updated: interval %.2f ms, latency %u, timeout %u ms\n",
                       interval * 1.25f, latency, timeout * 10);
            }
            break;
        }

        default:
            break;
    }
}
