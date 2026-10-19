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

private:
    bool initialized_;
    wl_handle_t wl_handle_;
};
