#pragma once

#include "wear_levelling.h"

class UsbMsc {
public:
    UsbMsc();
    ~UsbMsc();

    // Initializes TinyUSB composite device (CDC ACM serial and MSC flash drive).
    bool init(wl_handle_t wl_handle);

    // Returns true if the USB device is mounted by the host.
    bool is_mounted() const;

private:
    bool initialized_;
};
