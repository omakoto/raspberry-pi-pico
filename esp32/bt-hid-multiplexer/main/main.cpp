#include <stdio.h>

#include "app_task.h"
#include "config.h"
#include "dual_console.h"
#include "log_ring.h"
#include "platform.h"
#include "ui_task.h"

extern "C" void app_main(void) {
    // First: from here on all console output goes through the log ring (see dual_console.h).
    dual_console_init();
    dual_console_start_task();

    print_welcome_banner();
    printf("[System] Reset reason: %s\n", platform_reset_reason_str());
    if (LogRing::previousRunValid() && LogRing::previousRunWasWatchdog()) {
        print_previous_run_report(false);
    }

    ui_task_start();
    app_task_start();
    // app_main returns; the tasks it started keep running.
}
