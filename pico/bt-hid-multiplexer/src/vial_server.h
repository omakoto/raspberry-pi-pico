#ifndef VIAL_SERVER_H_
#define VIAL_SERVER_H_

#include <stdint.h>
#include <stddef.h>

class VialServer {
public:
    static void init();
    
    // Process 32-byte bidirectional vendor RawHID packets (VIAL / VIA protocol)
    static void handleRawReport(const uint8_t *in_buf, uint8_t *out_buf);

    // Set once the host has sent the VIA "jump to bootloader" command. The caller reboots into USB
    // BOOTSEL mode after the reply has been sent. This is how 01-install.sh flashes the board when
    // the USB serial port (which it otherwise uses for that) is disabled.
    static bool bootloaderRequested();

private:
    static bool bootloader_requested_;
    static void handleViaCommand(const uint8_t *in_buf, uint8_t *out_buf);
    static void handleDebugCommand(const uint8_t *in_buf, uint8_t *out_buf);
    static void handleVialCommand(const uint8_t *in_buf, uint8_t *out_buf);
};

#endif // VIAL_SERVER_H_
