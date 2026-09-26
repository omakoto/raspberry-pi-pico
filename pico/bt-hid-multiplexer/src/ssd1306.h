#ifndef SSD1306_H_
#define SSD1306_H_

#include <stdint.h>
#include <stdbool.h>
#include "hardware/i2c.h"
#include "config.h"

class SSD1306 {
public:
    static void init();
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
    static void renderStatus(bool ble_connected, const char *dev_name, int active_layer, 
                             bool pairing_active, uint32_t passkey, const char *toast_msg);

private:
    static uint8_t buffer_[OLED_WIDTH * OLED_HEIGHT / 8];
    static void writeCmd(uint8_t cmd);
    static void writeCmdList(const uint8_t *cmds, size_t len);
};

#endif // SSD1306_H_
