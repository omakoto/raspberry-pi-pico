#include <stdio.h>

#include "app_task.h"
#include "config.h"
#include "platform.h"

extern "C" void app_main(void) {
    printf("\n=== BLE HID Multiplexer v%s on %s ===\n", FIRMWARE_VERSION, BOARD_NAME);
    printf("Reset reason: %s\n", platform_reset_reason_str());

    app_task_start();
    // app_main returns; the tasks it started keep running.
}
