#ifndef USB_HOST_MANAGER_H_
#define USB_HOST_MANAGER_H_

#include <stdint.h>
#include <stdbool.h>
#include "config.h"

class UsbHostManager {
public:
    static void init();
    static void poll();
    static void resetBus();
    static void printDiagnostics();
    static bool isKeyboardConnected(uint8_t dev_idx);
    static bool isMouseConnected(uint8_t dev_idx);
    static uint8_t getConnectedKeyboardCount();
    static uint8_t getConnectedMouseCount();

private:
    static bool prev_kbd_connected_[MAX_KEYBOARDS];
    static bool prev_mouse_connected_[MAX_MICE];
};

#endif // USB_HOST_MANAGER_H_
