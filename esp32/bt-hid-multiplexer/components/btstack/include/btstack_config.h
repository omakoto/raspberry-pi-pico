#ifndef BTSTACK_CONFIG_H
#define BTSTACK_CONFIG_H

// BTstack configuration of the ESP32-S3 BLE HID multiplexer: a BLE central / HID-over-GATT host on
// the ESP32-S3's BLE-only controller (VHCI). The BLE settings match the Pico build; the buffer and
// port settings come from BTstack's port/esp32 configuration.

// Port related features
#define HAVE_ASSERT
#define HAVE_EMBEDDED_TIME_MS
#define HAVE_FREERTOS_INCLUDE_PREFIX
#define HAVE_FREERTOS_TASK_NOTIFICATIONS
#define HAVE_MALLOC

// BTstack features for BLE Central (HOGP Host)
#define ENABLE_BLE
#define ENABLE_LE_CENTRAL
#define ENABLE_LE_PERIPHERAL
#define ENABLE_L2CAP_LE_CREDIT_BASED_FLOW_CONTROL_MODE
#define ENABLE_LE_DATA_LENGTH_EXTENSION

// Locate CCCDs with ATT Read By Type instead of the default Find Information walk. BTstack's
// Find Information handler loses the CCCD write when a peripheral spreads a characteristic's
// descriptors over several responses (small ATT MTU, one descriptor per reply): it finds the
// CCCD, then continues the walk and never issues the write, so every notification enable ends
// in a 30 s GATT timeout. The ProtoArc XK01 keyboard (MTU 23) triggers exactly this.
#define ENABLE_GATT_LEGACY_CCC_DISCOVERY

// Diagnostic logging
#define ENABLE_LOG_INFO
#define ENABLE_LOG_ERROR
#define ENABLE_PRINTF_HEXDUMP

// Concurrent connection limits (support up to 8 BLE HID peripherals)
#define MAX_NR_GATT_CLIENTS 8
#define MAX_NR_HCI_CONNECTIONS 8
#define MAX_NR_HIDS_CLIENTS 8
#define MAX_NR_L2CAP_CHANNELS 16
#define MAX_NR_L2CAP_SERVICES 8
#define MAX_NR_SM_LOOKUP_ENTRIES 8
#define MAX_NR_WHITELIST_ENTRIES 8
#define MAX_NR_LE_DEVICE_DB_ENTRIES 16

// HCI buffers. BLE needs no BR/EDR-sized ACL payloads; this allows 251-byte LE data length packets.
#define HCI_ACL_PAYLOAD_SIZE (255 + 4)

// Controller to host flow control. The port's VHCI receive ring is sized from these and drops
// packets when it is full.
#define ENABLE_HCI_CONTROLLER_TO_HOST_FLOW_CONTROL
#define HCI_HOST_ACL_PACKET_LEN HCI_ACL_PAYLOAD_SIZE
#define HCI_HOST_ACL_PACKET_NUM 20
#define HCI_HOST_SCO_PACKET_LEN 0
#define HCI_HOST_SCO_PACKET_NUM 0

// Bonding storage (TLV in NVS, namespace "BTstack")
#define NVM_NUM_DEVICE_DB_ENTRIES 16
#define MAX_ATT_DB_SIZE 512

// Security and cryptography
#define ENABLE_SOFTWARE_AES128
#define ENABLE_LE_SECURE_CONNECTIONS
#define ENABLE_MICRO_ECC_FOR_LE_SECURE_CONNECTIONS

#endif // BTSTACK_CONFIG_H
