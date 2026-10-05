#include <stdio.h>

#include "app_task.h"
#include "config.h"
#include "log_ring.h"
#include "platform.h"

extern "C" void app_main(void) {
    printf("\n=== BLE HID Multiplexer v%s on %s ===\n", FIRMWARE_VERSION, BOARD_NAME);
    printf("Reset reason: %s\n", platform_reset_reason_str());

    // Keeps the previous run's log if it survived the reset. Nothing drains the ring to a console yet.
    LogRing::init(nullptr, platform_reset_was_crash());

    app_task_start();
    // app_main returns; the tasks it started keep running.
}
