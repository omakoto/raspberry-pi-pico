#ifndef VIAL_SERVER_H_
#define VIAL_SERVER_H_

#include <stdint.h>
#include <stdbool.h>

class Stream;

class VialServer {
public:
    static void init();
    static void poll();
    static void processSerialCommand(const char *cmd_line, Stream &stream);
    static void processSerialCommand(const char *cmd_line);

private:
    static void handleRawHidPacket(const uint8_t *in_buf, uint8_t *out_buf);
    static void handleViaCommand(const uint8_t *in_buf, uint8_t *out_buf);
    static void handleVialCommand(const uint8_t *in_buf, uint8_t *out_buf);
};

#endif // VIAL_SERVER_H_
