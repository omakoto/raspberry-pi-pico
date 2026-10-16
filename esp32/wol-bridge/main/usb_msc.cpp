#include "usb_msc.hpp"
#include "esp_log.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_msc.h"
#include "tusb.h"

static const char* TAG = "UsbMsc";

UsbMsc::UsbMsc() : initialized_(false) {}

UsbMsc::~UsbMsc() {}

#include "soc/rtc_cntl_reg.h"
#include "esp_system.h"

static void cdc_line_coding_changed(int itf, cdcacm_event_t *event) {
    if (event->line_coding_changed_data.p_line_coding->bit_rate == 1200) {
        ESP_LOGI("UsbMsc", "1200 baud touch detected! Rebooting into download mode...");
        vTaskDelay(pdMS_TO_TICKS(50));
        REG_CLR_BIT(RTC_CNTL_USB_CONF_REG, RTC_CNTL_SW_HW_USB_PHY_SEL | RTC_CNTL_SW_USB_PHY_SEL);
        REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
        esp_restart();
    }
}

bool UsbMsc::init(wl_handle_t wl_handle) {
    ESP_LOGI(TAG, "Initializing TinyUSB Composite Device (CDC ACM + MSC Storage)...");

    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    esp_err_t ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install TinyUSB driver: %s", esp_err_to_name(ret));
        return false;
    }

    // Initialize CDC ACM interface for serial console
    tinyusb_config_cdcacm_t acm_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = nullptr,
        .callback_rx_wanted_char = nullptr,
        .callback_line_state_changed = nullptr,
        .callback_line_coding_changed = &cdc_line_coding_changed
    };
    esp_err_t cdc_ret = tinyusb_cdcacm_init(&acm_cfg);
    if (cdc_ret == ESP_OK) {
        ESP_LOGI(TAG, "TinyUSB CDC ACM serial interface initialized");
    } else {
        ESP_LOGW(TAG, "Failed to initialize TinyUSB CDC ACM: %s", esp_err_to_name(cdc_ret));
    }

    // Initialize MSC Storage for FATFS wear-levelling partition
    if (wl_handle != WL_INVALID_HANDLE) {
        const tinyusb_msc_storage_config_t msc_cfg = {
            .medium = {
                .wl_handle = wl_handle
            },
            .fat_fs = {
                .base_path = nullptr,
                .config = {
                    .format_if_mount_failed = false,
                    .max_files = 2,
                    .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
                    .disk_status_check_enable = false,
                    .use_one_fat = false
                },
                .do_not_format = true,
                .format_flags = 0
            },
            .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB
        };
        tinyusb_msc_storage_handle_t msc_handle;
        esp_err_t msc_ret = tinyusb_msc_new_storage_spiflash(&msc_cfg, &msc_handle);
        if (msc_ret == ESP_OK) {
            ESP_LOGI(TAG, "TinyUSB MSC Storage initialized for FATFS partition");
        } else {
            ESP_LOGW(TAG, "Failed to initialize TinyUSB MSC Storage: %s", esp_err_to_name(msc_ret));
        }
    }

    initialized_ = true;
    ESP_LOGI(TAG, "TinyUSB Composite CDC + MSC driver installed successfully");
    return true;
}

bool UsbMsc::is_mounted() const {
    return tud_mounted();
}
