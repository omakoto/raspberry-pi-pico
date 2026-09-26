#ifndef VIAL_SERVER_H_
#define VIAL_SERVER_H_

#include <stdint.h>
#include <stddef.h>

class VialServer {
public:
    static void init();
    
    // Process 32-byte bidirectional vendor RawHID packets (VIAL / VIA protocol)
    static void handleRawReport(const uint8_t *in_buf, uint8_t *out_buf);

private:
    static void handleViaCommand(const uint8_t *in_buf, uint8_t *out_buf);
    static void handleVialCommand(const uint8_t *in_buf, uint8_t *out_buf);
};

#endif // VIAL_SERVER_H_
