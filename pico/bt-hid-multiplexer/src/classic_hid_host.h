#ifndef CLASSIC_HID_HOST_H_
#define CLASSIC_HID_HOST_H_

#include <stdint.h>
#include <stdbool.h>

// Bluetooth Classic (BR/EDR) HID host. Complements BleHidHost for keyboards, keypads and mice that
// only speak the classic HID profile (L2CAP PSM 0x11/0x13), which BLE scanning can never see.
// Reports are fed into the same Multiplexer as the BLE devices, with device indices following the
// BLE ones (MAX_BLE_DEVICES + slot).
class ClassicHidHost {
public:
    // Must be called after l2cap_init() and before the controller is powered on.
    static void init();

    // Pairing mode drives an inquiry (discovery) for peripheral-class devices. Outside of it the
    // host is only connectable, so bonded devices can reconnect on their own.
    static void startPairingMode();
    static void stopPairingMode();

    static uint8_t getConnectedCount();
    static const char* getConnectedDeviceName();
    static uint32_t getActivePasskey();

    // Mirror the host PC's lock LEDs to connected classic keyboards.
    static void sendHostLeds(uint8_t leds);

    // Console helpers.
    static void dumpDevices();
    static void dumpBonds();
    static void connectBonded(uint8_t bond_idx);
    static void disconnectSlot(uint8_t slot_idx);
    static void clearBonds();
};

#endif // CLASSIC_HID_HOST_H_
