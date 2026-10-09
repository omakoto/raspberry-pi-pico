#ifndef UI_TASK_H_
#define UI_TASK_H_

#include <stdint.h>
#include <stdbool.h>

// What the OLED shows. The bt_app task, which owns this state, publishes it; the
// UI task renders it. The UI task blocks on I2C (a full OLED frame takes ~25 ms at 400 kHz), so it
// must not be the one holding application state.
struct UiSnapshot {
    uint8_t connected_count;
    char device_name[32];
    int active_layer;
    bool pairing;
    uint32_t passkey;
    char toast[32];  // empty when there is none; bt_app drops it when it expires
    bool usb_mounted;
};

// Starts the UI task (OLED with its boot splash).
void ui_task_start();

// Publishes a new snapshot if it differs from the last one; bt_app task only.
void ui_publish(const UiSnapshot &snapshot);

#endif // UI_TASK_H_
