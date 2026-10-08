#ifndef OLED_H_
#define OLED_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "config.h"

// Driver for a 128x64 I2C OLED with an SSD1306 or SH1106 controller (OLED_CONTROLLER in config.h).
class Oled {
public:
    // Returns false (and leaves every other call a no-op) if no display answers on the I2C bus.
    static bool init();
    static void clear(bool color = false);
    static void pixel(int x, int y, bool color = true);
    static void line(int x0, int y0, int x1, int y1, bool color = true);
    static void rect(int x, int y, int w, int h, bool color = true, bool fill = false);
    
    // Character and string rendering using built-in 8x16 or compact 4x5 fonts
    static int drawChar(char c, int x, int y, bool color = true, bool font_large = false);
    static void drawString(const char *s, int x, int y, bool color = true, bool font_large = false);
    
    // Push the local 1024-byte framebuffer to the display controller over I2C
    static void show();

    // High-level UI status screen
    // dev_name: the device used last (or connected last); connected_count == 0 shows the
    // DISCONNECTED screen.
    static void renderStatus(uint8_t connected_count, const char *dev_name, int active_layer,
                             bool pairing_active, uint32_t passkey, const char *toast_msg, bool usb_mounted);
    static void renderBootSplash(const char *board_desc, const char *version_desc);

    // Probes every 7-bit address on the OLED's I2C bus and stores the ones that ACK in found (up to
    // max). Returns the number stored, or -1 if the bus could not be set up. For diagnosing a display
    // that does not answer at OLED_I2C_ADDR.
    static int scanBus(uint8_t *found, int max);

private:
    static uint8_t buffer_[OLED_WIDTH * OLED_HEIGHT / 8];
    static void write(const uint8_t *data, size_t len);
    static void writeCmd(uint8_t cmd);
    static void writeCmdList(const uint8_t *cmds, size_t len);
};

#endif // OLED_H_
