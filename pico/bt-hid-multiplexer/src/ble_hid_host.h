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
    static const char* getConnectedDeviceName();
    static uint32_t getActivePasskey();
    static void clearPasskey();
    static void clearBonds();
    static void sendHostLeds(uint8_t leds);

    // Callbacks for BTstack run loop
    static void packetHandler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
    static void smPacketHandler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
    static void gattPacketHandler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
};

#endif // BLE_HID_HOST_H_
