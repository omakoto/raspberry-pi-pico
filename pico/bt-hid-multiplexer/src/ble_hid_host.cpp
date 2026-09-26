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

static uint8_t s_hid_descriptor_storage[512];
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

static bool adv_has_hid_service(const uint8_t *packet) {
    const uint8_t *ad_data = gap_event_advertising_report_get_data(packet);
    uint8_t ad_len = gap_event_advertising_report_get_data_length(packet);
    return ad_data_contains_uuid16(ad_len, ad_data, ORG_BLUETOOTH_SERVICE_HUMAN_INTERFACE_DEVICE);
}

static void extract_device_name(const uint8_t *packet, char *out_name, size_t max_len) {
    const uint8_t *ad_data = gap_event_advertising_report_get_data(packet);
    uint8_t ad_len = gap_event_advertising_report_get_data_length(packet);
    
    // Look for Complete Local Name (0x09) or Shortened Local Name (0x08)
    for (uint8_t i = 0; i < ad_len;) {
        uint8_t len = ad_data[i];
        if (len == 0 || (i + len) > ad_len) break;
        uint8_t type = ad_data[i + 1];
        if (type == 0x09 || type == 0x08) {
            uint8_t name_len = len - 1;
            if (name_len >= max_len) name_len = max_len - 1;
            memcpy(out_name, &ad_data[i + 2], name_len);
            out_name[name_len] = '\0';
            return;
        }
        i += len + 1;
    }
    // Fall back to MAC string if no advertisement name
    bd_addr_t addr;
    gap_event_advertising_report_get_address(packet, addr);
    snprintf(out_name, max_len, "%02X:%02X:%02X:%02X:%02X:%02X",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
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

    // Passive continuous scanning when active
    gap_set_scan_parameters(0, 48, 48);

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
    if (packet_type != HCI_EVENT_PACKET) return;

    switch (hci_event_packet_get_type(packet)) {
        case GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED: {
            uint8_t status = gattservice_subevent_hid_service_connected_get_status(packet);
            if (status == ERROR_CODE_SUCCESS) {
                printf("[BLE Host] HID Service Connected successfully!\n");
                s_connected = true;
                s_active_passkey = 0;
            } else {
                printf("[BLE Host] HID Service connection failed, status: 0x%02X\n", status);
                s_connected = false;
            }
            break;
        }

        case GATTSERVICE_SUBEVENT_HID_SERVICE_DISCONNECTED:
            printf("[BLE Host] HID Service Disconnected.\n");
            s_connected = false;
            Multiplexer::purgeKeyboard(0);
            Multiplexer::purgeMouse(0);
            break;

        case GATTSERVICE_SUBEVENT_HID_REPORT: {
            const uint8_t *report = gattservice_subevent_hid_report_get_report(packet);
            uint16_t len = gattservice_subevent_hid_report_get_report_len(packet);
            if (!report || len == 0) break;

            if (len == 8) {
                // Standard Boot Keyboard report: [mods, reserved, key1..6]
                Multiplexer::handleKeyboardReport(0, report[0], &report[2], 6);
            } else if (len >= 3 && len <= 5) {
                // Boot/Report Mouse report: [buttons, dx, dy, optional wheel]
                int8_t wheel = (len >= 4) ? (int8_t)report[3] : 0;
                Multiplexer::handleMouseReport(0, report[0], (int8_t)report[1], (int8_t)report[2], wheel);
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
        printf("[BLE Host] Connecting HIDS client...\n");
        hids_client_connect(s_con_handle, &gattPacketHandler, HID_PROTOCOL_MODE_BOOT, &s_hids_cid);
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
            }
            break;

        case GAP_EVENT_ADVERTISING_REPORT:
            if (!s_is_scanning) break;
            if (!adv_has_hid_service(packet)) break;

            // Found BLE HID device during active scan
            gap_stop_scan();
            s_is_scanning = false;

            gap_event_advertising_report_get_address(packet, s_remote_addr);
            s_remote_addr_type = (bd_addr_type_t)gap_event_advertising_report_get_address_type(packet);
            extract_device_name(packet, s_connected_dev_name, sizeof(s_connected_dev_name));

            printf("[BLE Host] Discovered HID device '%s' (%s). Connecting...\n",
                   s_connected_dev_name, bd_addr_to_str(s_remote_addr));
            gap_connect(s_remote_addr, s_remote_addr_type);
            break;

        case HCI_EVENT_DISCONNECTION_COMPLETE:
            printf("[BLE Host] Device disconnected.\n");
            s_con_handle = HCI_CON_HANDLE_INVALID;
            s_connected = false;
            s_active_passkey = 0;
            s_connected_dev_name[0] = '\0';
            Multiplexer::purgeKeyboard(0);
            Multiplexer::purgeMouse(0);
            break;

        case HCI_EVENT_META_GAP:
            if (hci_event_gap_meta_get_subevent_code(packet) == GAP_SUBEVENT_LE_CONNECTION_COMPLETE) {
                s_con_handle = gap_subevent_le_connection_complete_get_connection_handle(packet);
                printf("[BLE Host] LE Connection established (handle 0x%04X). Requesting security/pairing...\n", s_con_handle);
                sm_request_pairing(s_con_handle);
            }
            break;

        default:
            break;
    }
}
