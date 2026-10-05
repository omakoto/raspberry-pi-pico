#include "platform.h"

#include "esp_system.h"
#include "esp_timer.h"

uint32_t platform_now_ms() {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

const char *platform_reset_reason_str() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   return "power-on";
        case ESP_RST_EXT:       return "external pin";
        case ESP_RST_SW:        return "software restart";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "interrupt watchdog";
        case ESP_RST_TASK_WDT:  return "task watchdog";
        case ESP_RST_WDT:       return "other watchdog";
        case ESP_RST_DEEPSLEEP: return "deep sleep wake-up";
        case ESP_RST_BROWNOUT:  return "brown-out";
        case ESP_RST_SDIO:      return "SDIO";
        case ESP_RST_USB:       return "USB peripheral";
        case ESP_RST_JTAG:      return "JTAG";
        default:                return "unknown";
    }
}

bool platform_reset_was_crash() {
    switch (esp_reset_reason()) {
        case ESP_RST_PANIC:
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:
            return true;
        default:
            return false;
    }
}
