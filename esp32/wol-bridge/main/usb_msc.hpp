#pragma once

#include "wear_levelling.h"

class UsbMsc {
public:
    UsbMsc();
    ~UsbMsc();

    // Initializes TinyUSB composite device (CDC ACM serial and MSC flash drive).
    // The FATFS partition must not be mounted by anyone else: the MSC driver takes ownership of it
    // and switches it between the USB host and the application (at base_path) on its own.
    bool init(const char* partition_label = "storage", const char* base_path = "/spiflash");

    // Returns true if the USB device is mounted by the host.
    bool is_mounted() const;

    // Restarts into the ROM download mode, so that the board can be flashed over this USB port.
    [[noreturn]] static void reboot_to_download_mode();

private:
    bool initialized_;
    wl_handle_t wl_handle_;
};
