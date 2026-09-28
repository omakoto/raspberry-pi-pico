#include "ble_hid_host.h"
#include "multiplexer.h"
#include "config.h"
#include "btstack.h"
#include "ble/gatt-service/hids_client.h"
#include "ble/le_device_db.h"
#include "pico/time.h"
#include <stdio.h>
#include <string.h>

static bool s_is_scanning = false;
static bool s_connected = false;
static uint32_t s_active_passkey = 0;
static char s_connected_dev_name[32] = {0};

static bd_addr_t s_remote_addr;
static bd_addr_type_t s_remote_addr_type = BD_ADDR_TYPE_LE_PUBLIC;
static hci_con_handle_t s_con_handle = HCI_CON_HANDLE_INVALID;
static uint16_t s_hids_cid = 0;
static hid_protocol_mode_t s_protocol_mode = HID_PROTOCOL_MODE_REPORT;

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
            strstr(lower, "trackpad") != NULL ||
            strstr(lower, "touchpad") != NULL ||
            strstr(lower, "mouse")    != NULL ||
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
static SeenDevice s_seen_devices[16];
static uint8_t s_seen_idx = 0;

// Updates seen device records across advertisement and scan response packets,
// and throttles discovery log messages per device address
static bool update_and_should_log_device(const bd_addr_t addr, uint32_t now, AdvDeviceInfo *info) {
    for (uint8_t i = 0; i < 16; ++i) {
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
    s_seen_idx = (s_seen_idx + 1) % 16;
    return true;
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

    s_is_scanning = false;
    s_connected = false;
    s_active_passkey = 0;
    s_con_handle = HCI_CON_HANDLE_INVALID;
    s_connected_dev_name[0] = '\0';

    hci_power_control(HCI_POWER_ON);
}

void BleHidHost::startScan() {
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
    return s_connected_dev_name;
}

uint32_t BleHidHost::getActivePasskey() {
    return s_active_passkey;
}

void BleHidHost::clearPasskey() {
    s_active_passkey = 0;
}

void BleHidHost::clearBonds() {
    printf("[BLE Host] Clearing all bonded devices.\n");
    for (int i = le_device_db_count() - 1; i >= 0; i--) {
        le_device_db_remove(i);
    }
    if (s_con_handle != HCI_CON_HANDLE_INVALID) {
        gap_disconnect(s_con_handle);
    }
}

void BleHidHost::sendHostLeds(uint8_t leds) {
    if (s_connected && s_hids_cid != 0) {
        // Send Output Report containing keyboard LED status
        uint8_t led_report = leds;
        hids_client_send_write_report(s_hids_cid, 0, HID_REPORT_TYPE_OUTPUT, &led_report, 1);
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
                // Explicitly ensure report notifications are enabled across all discovered input reports
                hids_client_enable_notifications(s_hids_cid);
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

            printf("[BLE Host] HID Report (id %u, len %u):", report_id, data_len);
            for (uint16_t i = 0; i < data_len && i < 16; i++) {
                printf(" %02X", data[i]);
            }
            printf("\n");

            if (data_len == 8) {
                // Standard Keyboard report: [modifiers, reserved, key1..6]
                Multiplexer::handleKeyboardReport(0, data[0], &data[2], 6);
            } else if (data_len == 7) {
                // Keyboard report without reserved byte: [modifiers, key1..6]
                Multiplexer::handleKeyboardReport(0, data[0], &data[1], 6);
            } else if (data_len >= 3 && data_len <= 5) {
                // Mouse report: [buttons, dx, dy, optional wheel]
                int8_t wheel = (data_len >= 4) ? (int8_t)data[3] : 0;
                Multiplexer::handleMouseReport(0, data[0], (int8_t)data[1], (int8_t)data[2], wheel);
            } else {
                printf("[BLE Host] Unhandled HID Report format (id %u, data_len %u)\n", report_id, data_len);
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

    bool connect_hids = false;

    switch (hci_event_packet_get_type(packet)) {
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
            if (status == ERROR_CODE_SUCCESS) {
                printf("[BLE Host] Pairing complete: SUCCESS\n");
                s_active_passkey = 0;
                connect_hids = true;
            } else {
                printf("[BLE Host] Pairing complete: FAILED (status 0x%02X)\n", status);
                s_active_passkey = 0;
                if (s_con_handle != HCI_CON_HANDLE_INVALID) {
                    gap_disconnect(s_con_handle);
                }
            }
            break;
        }

        case SM_EVENT_REENCRYPTION_COMPLETE:
            printf("[BLE Host] Re-encryption complete.\n");
            connect_hids = true;
            break;

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
                startScan();
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

            if (is_target_hid_device(info)) {
                gap_stop_scan();
                s_is_scanning = false;

                bd_addr_copy(s_remote_addr, addr);
                s_remote_addr_type = (bd_addr_type_t)addr_type;
                if (info.name[0] != '\0') {
                    strncpy(s_connected_dev_name, info.name, sizeof(s_connected_dev_name) - 1);
                    s_connected_dev_name[sizeof(s_connected_dev_name) - 1] = '\0';
                } else {
                    snprintf(s_connected_dev_name, sizeof(s_connected_dev_name), "%s", bd_addr_to_str(addr));
                }

                printf("[BLE Host] Target HID device found: '%s' (%s). Connecting...\n",
                       s_connected_dev_name, bd_addr_to_str(s_remote_addr));
                gap_connect(s_remote_addr, s_remote_addr_type);
            }
            break;
        }

        case HCI_EVENT_DISCONNECTION_COMPLETE:
            printf("[BLE Host] Device disconnected. Resuming scan...\n");
            s_con_handle = HCI_CON_HANDLE_INVALID;
            s_connected = false;
            s_active_passkey = 0;
            s_connected_dev_name[0] = '\0';
            Multiplexer::purgeKeyboard(0);
            Multiplexer::purgeMouse(0);
            startScan();
            break;

        case HCI_EVENT_META_GAP:
            if (hci_event_gap_meta_get_subevent_code(packet) == GAP_SUBEVENT_LE_CONNECTION_COMPLETE) {
                uint8_t status = gap_subevent_le_connection_complete_get_status(packet);
                if (status != ERROR_CODE_SUCCESS) {
                    printf("[BLE Host] LE Connection failed (status 0x%02X). Resuming scan...\n", status);
                    s_con_handle = HCI_CON_HANDLE_INVALID;
                    s_connected = false;
                    s_active_passkey = 0;
                    startScan();
                    break;
                }
                s_con_handle = gap_subevent_le_connection_complete_get_connection_handle(packet);
                printf("[BLE Host] LE Connection established (handle 0x%04X). Requesting security/pairing...\n", s_con_handle);
                sm_request_pairing(s_con_handle);
            }
            break;

        default:
            break;
    }
}
