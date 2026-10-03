#ifndef BLE_HID_HOST_H_
#define BLE_HID_HOST_H_

#include <stdint.h>
#include <stdbool.h>

class BleHidHost {
public:
    static void init();
    static void startScan();
    static void stopScan();
    static bool isScanning();
    static bool isConnected();
    static uint8_t getConnectedCount();
    static uint8_t getBondedCount();
    static bool hasUnconnectedBonds();
    static const char* getConnectedDeviceName();
    static const char* getConnectedDeviceName(uint8_t slot_idx);
    static uint32_t getActivePasskey();
    static void clearPasskey();
    static void clearBonds();
    static void dumpBonds();
    static void dumpDescriptor();
    static void dumpDevices();
    static void sendHostLeds(uint8_t leds);

    // Diagnostics and peripheral control
    static void unbond(uint8_t idx);
    static void disconnectSlot(uint8_t slot_idx);
    static void sendExitSuspend(uint8_t slot_idx);
    static void sendSetProtocolMode(uint8_t slot_idx, uint8_t mode);
    static void sendGetReport(uint8_t slot_idx, uint8_t report_id);
    static void enableNotifications(uint8_t slot_idx);

    // Callbacks for BTstack run loop
    static void packetHandler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
    static void smPacketHandler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
    static void gattPacketHandler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
};

#endif // BLE_HID_HOST_H_
