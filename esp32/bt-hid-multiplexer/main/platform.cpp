#include "platform.h"

#include "esp_system.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/rtc_cntl_reg.h"
#include "esp_timer.h"

static portMUX_TYPE s_critical_lock = portMUX_INITIALIZER_UNLOCKED;

void platform_critical_enter() {
    portENTER_CRITICAL_SAFE(&s_critical_lock);
}

void platform_critical_exit() {
    portEXIT_CRITICAL_SAFE(&s_critical_lock);
}

void platform_reboot_to_download_mode() {
    // The calling task must not be reset by the task watchdog while the reboot is under way.
    esp_task_wdt_delete(NULL);
    // TinyUSB has routed the internal USB PHY to the USB OTG controller. That routing is in the RTC
    // domain, which a restart does not reset, and the ROM's download mode talks over the USB-Serial-
    // JTAG controller: hand the PHY back to the hardware default (USB-Serial-JTAG) so that the
    // download port appears on the native USB port.
    REG_CLR_BIT(RTC_CNTL_USB_CONF_REG, RTC_CNTL_SW_HW_USB_PHY_SEL | RTC_CNTL_SW_USB_PHY_SEL);
    // Makes the ROM bootloader stay in download mode after the restart (what ESP-IDF itself does for
    // its USB console's 1200-baud reset).
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
}

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
