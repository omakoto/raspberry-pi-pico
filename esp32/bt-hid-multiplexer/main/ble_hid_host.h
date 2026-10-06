#ifndef BLE_HID_HOST_H_
#define BLE_HID_HOST_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

class BleHidHost {
public:
    static void init();
    static void startScan();
    static void stopScan();
    static bool isScanning();
    static void startPairingMode(uint32_t timeout_ms = 60000);
    static void stopPairingMode();
    static bool isPairingMode();
    static uint32_t getPairingModeRemainingSec();
    static bool isConnected();
    static uint8_t getConnectedCount();
    static uint8_t getBondedCount();
    // Address and name of bonded device idx (0 .. getBondedCount() - 1).
    static bool getBondedDevice(uint8_t idx, uint8_t addr[6], char *name, size_t name_size);
    // Whether the device with this address is bonded, and if so its name.
    static bool findBondedDevice(const uint8_t addr[6], char *name, size_t name_size);
    static bool hasUnconnectedBonds();
    static const char* getConnectedDeviceName();
    static const char* getConnectedDeviceName(uint8_t slot_idx);
    // The connected slot whose HID service came up last, or 0xFF if none is connected.
    static uint8_t getMostRecentlyConnectedSlot();
    // Bluetooth address of the connected device with this multiplexer device index.
    static bool getSlotAddress(uint8_t dev_idx, uint8_t addr[6]);
    static uint32_t getActivePasskey();
    static void clearPasskey();
    static void clearBonds();
    static void dumpBonds();
    static void dumpDescriptor();
    static void dumpDevices();
    static void sendHostLeds(uint8_t leds);

    // Mouse sensitivity scaling (percent, e.g. 100 = 100%, 50 = 50%, 25 = 25%)
    static void setMouseSpeed(uint8_t slot_idx, uint16_t percent);
    static uint16_t getMouseSpeed(uint8_t slot_idx);
    static void setGlobalMouseSpeed(uint16_t percent);
    static uint16_t getGlobalMouseSpeed();

    // Diagnostics and peripheral control
    static void unbond(uint8_t idx);
    static void disconnectSlot(uint8_t slot_idx);
    static void sendExitSuspend(uint8_t slot_idx);
    static void sendSetProtocolMode(uint8_t slot_idx, uint8_t mode);
    static void sendGetReport(uint8_t slot_idx, uint8_t report_id);
    static void enableNotifications(uint8_t slot_idx);
    static void requestProtocolMode(uint8_t slot_idx);
    // Request new LL connection parameters; interval in 1.25 ms units (0 = keep current)
    static void updateConnectionParams(uint8_t slot_idx, uint16_t interval_units, uint16_t latency);

    // Pairing policy (applies to new pairings only) and BTstack internal logging
    static void setAuthReq(bool mitm, bool secure_connections);
    static void dumpAuthReq();
    static void setStackLogging(bool enable);
    static bool isStackLogging();
    static void setHciPacketLogging(bool enable);
    static void setReportLogging(bool enable);

    // Callbacks for BTstack run loop
    static void packetHandler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
    static void smPacketHandler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
    static void gattPacketHandler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
};

#endif // BLE_HID_HOST_H_
