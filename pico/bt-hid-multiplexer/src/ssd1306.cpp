#include "ssd1306.h"
#include "font_8x16.h"
#include "font_4x5.h"
#include "hardware/gpio.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

uint8_t SSD1306::buffer_[OLED_WIDTH * OLED_HEIGHT / 8];

void SSD1306::writeCmd(uint8_t cmd) {
    uint8_t payload[2] = {0x00, cmd};
    i2c_write_blocking(I2C_PORT_OLED, OLED_I2C_ADDR, payload, 2, false);
}

void SSD1306::writeCmdList(const uint8_t *cmds, size_t len) {
    uint8_t payload[32];
    payload[0] = 0x00;
    while (len > 0) {
        size_t chunk = (len > 31) ? 31 : len;
        memcpy(&payload[1], cmds, chunk);
        i2c_write_blocking(I2C_PORT_OLED, OLED_I2C_ADDR, payload, chunk + 1, false);
        cmds += chunk;
        len -= chunk;
    }
}

void SSD1306::init() {
    // Initialize I2C peripheral at 400 kHz fast-mode
    i2c_init(I2C_PORT_OLED, OLED_BAUDRATE_HZ);
    gpio_set_function(PIN_OLED_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_OLED_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(PIN_OLED_SDA);
    gpio_pull_up(PIN_OLED_SCL);

    // Standard 128x64 SSD1306 initialization sequence
    const uint8_t init_cmds[] = {
        0xAE,        // Display OFF
        0xD5, 0x80,  // Set display clock divide ratio / oscillator frequency
        0xA8, 0x3F,  // Set multiplex ratio (1 to 64)
        0xD3, 0x00,  // Set display offset to 0
        0x40,        // Set display start line to 0
        0x8D, 0x14,  // Enable charge pump regulator
        0x20, 0x00,  // Set memory addressing mode to Horizontal
        0xA1,        // Set segment re-map (COL127 mapped to SEG0)
        0xC8,        // Set COM Output Scan Direction (remap)
        0xDA, 0x12,  // Set COM pins hardware configuration
        0x81, 0xCF,  // Set contrast control to 0xCF
        0xD9, 0xF1,  // Set pre-charge period
        0xDB, 0x40,  // Set VCOMH deselect level
        0xA4,        // Entire display ON (resume to RAM content)
        0xA6,        // Set normal display (not inverted)
        0xAF         // Display ON
    };

    writeCmdList(init_cmds, sizeof(init_cmds));
    clear(false);
    show();
}

void SSD1306::clear(bool color) {
    memset(buffer_, color ? 0xFF : 0x00, sizeof(buffer_));
}

void SSD1306::pixel(int x, int y, bool color) {
    if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) {
        return;
    }
    int page = y / 8;
    int bit = y % 8;
    int idx = (page * OLED_WIDTH) + x;
    if (color) {
        buffer_[idx] |= (1 << bit);
    } else {
        buffer_[idx] &= ~(1 << bit);
    }
}

void SSD1306::line(int x0, int y0, int x1, int y1, bool color) {
    int dx = abs(x1 - x0);
    int dy = abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    while (true) {
        pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void SSD1306::rect(int x, int y, int w, int h, bool color, bool fill) {
    if (fill) {
        for (int i = 0; i < w; i++) {
            for (int j = 0; j < h; j++) {
                pixel(x + i, y + j, color);
            }
        }
    } else {
        line(x, y, x + w - 1, y, color);
        line(x, y + h - 1, x + w - 1, y + h - 1, color);
        line(x, y, x, y + h - 1, color);
        line(x + w - 1, y, x + w - 1, y + h - 1, color);
    }
}

int SSD1306::drawChar(char c, int x, int y, bool color, bool font_large) {
    if (font_large) {
        if (c < 0x20 || c > 0x7E) return 0;
        uint16_t offset = (uint16_t)(c - 0x20) * FONT_8X16_BYTES_PER_CHAR;
        const uint8_t *glyph = &FONT_8X16_DATA[offset];

        // Page-aligned fast blit optimization
        if ((y % 8) == 0 && x >= 0 && (x + FONT_8X16_GLYPH_WIDTH) <= OLED_WIDTH) {
            int start_page = y / 8;
            for (int p = 0; p < FONT_8X16_NUM_PAGES; p++) {
                int page = start_page + p;
                if (page < (OLED_HEIGHT / 8)) {
                    int buf_idx = (page * OLED_WIDTH) + x;
                    int glyph_idx = p * FONT_8X16_GLYPH_WIDTH;
                    for (int col = 0; col < FONT_8X16_GLYPH_WIDTH; col++) {
                        uint8_t val = glyph[glyph_idx + col];
                        if (color) {
                            buffer_[buf_idx + col] |= val;
                        } else {
                            buffer_[buf_idx + col] &= ~val;
                        }
                    }
                }
            }
            return FONT_8X16_CELL_WIDTH;
        }

        // Generic arbitrary coordinate fallback
        for (int p = 0; p < FONT_8X16_NUM_PAGES; p++) {
            int page_y = y + (p * 8);
            int glyph_idx = p * FONT_8X16_GLYPH_WIDTH;
            for (int col = 0; col < FONT_8X16_GLYPH_WIDTH; col++) {
                uint8_t val = glyph[glyph_idx + col];
                for (int row = 0; row < 8; row++) {
                    if (val & (1 << row)) {
                        pixel(x + col, page_y + row, color);
                    }
                }
            }
        }
        return FONT_8X16_CELL_WIDTH;
    } else {
        // Compact 4x5 font
        if (c >= 'a' && c <= 'z') c -= 32; // Uppercase fold
        if (c < 0x20 || c > 0x7E) return 0;
        uint16_t offset = (uint16_t)(c - 0x20) * FONT_4X5_BYTES_PER_CHAR;
        const uint8_t *glyph = &FONT_4X5_DATA[offset];

        for (int col = 0; col < FONT_4X5_GLYPH_WIDTH; col++) {
            uint8_t val = glyph[col];
            for (int row = 0; row < 8; row++) {
                if (val & (1 << row)) {
                    pixel(x + col, y + row, color);
                }
            }
        }
        return FONT_4X5_CELL_WIDTH;
    }
}

void SSD1306::drawString(const char *s, int x, int y, bool color, bool font_large) {
    if (!s) return;
    int curr_x = x;
    while (*s) {
        int width = drawChar(*s, curr_x, y, color, font_large);
        curr_x += width;
        s++;
    }
}

void SSD1306::show() {
    // Set column address range 0..127
    const uint8_t col_cmd[] = {0x21, 0x00, (uint8_t)(OLED_WIDTH - 1)};
    writeCmdList(col_cmd, sizeof(col_cmd));

    // Set page address range 0..7
    const uint8_t page_cmd[] = {0x22, 0x00, (uint8_t)((OLED_HEIGHT / 8) - 1)};
    writeCmdList(page_cmd, sizeof(page_cmd));

    // Send 1024-byte buffer in 128-byte chunks prefixed with data indicator 0x40
    uint8_t chunk_payload[129];
    chunk_payload[0] = 0x40; // Co=0, D/C#=1 (Data)

    for (size_t i = 0; i < sizeof(buffer_); i += 128) {
        memcpy(&chunk_payload[1], &buffer_[i], 128);
        i2c_write_blocking(I2C_PORT_OLED, OLED_I2C_ADDR, chunk_payload, 129, false);
    }
}

void SSD1306::renderStatus(bool ble_connected, const char *dev_name, int active_layer, 
                           bool pairing_active, uint32_t passkey, const char *toast_msg) {
    clear(false);

    // 1. Header Banner (0..11px)
    rect(0, 0, OLED_WIDTH, 11, true, true);
    if (pairing_active && ble_connected) {
        drawString("MULTIPLEXER [PAIR]", 4, 2, false, false);
    } else {
        drawString("PICO MULTIPLEXER", 4, 2, false, false);
    }

    // 2. Status or Pairing PIN (14..44px)
    if (passkey > 0) {
        // Prominent 6-digit Passkey PIN display
        char pin_str[16];
        snprintf(pin_str, sizeof(pin_str), "%06lu", (unsigned long)passkey);
        drawString("PAIR KEYBOARD PIN:", 2, 14, true, false);
        drawString(pin_str, 20, 24, true, true);
        drawString("Type PIN + Enter on KB", 2, 42, true, false);
    } else if (ble_connected) {
        drawString("BT:", 2, 16, true, false);
        drawString(dev_name ? dev_name : "Connected", 22, 16, true, false);

        char layer_str[32];
        const char *l_name = "BASE";
        if (active_layer == 1) l_name = "NAV/MEDIA";
        else if (active_layer == 2) l_name = "FUNCTION";
        else if (active_layer == 3) l_name = "NUMPAD";
        snprintf(layer_str, sizeof(layer_str), "LAYER %d: %s", active_layer, l_name);
        drawString(layer_str, 2, 28, true, false);

        if (toast_msg && toast_msg[0] != '\0') {
            drawString(toast_msg, 2, 40, true, false);
        } else if (pairing_active) {
            drawString("Pairing mode (60s)... | USB", 2, 40, true, false);
        } else {
            drawString("USB: Active | VIAL: Ready", 2, 40, true, false);
        }
    } else if (pairing_active) {
        drawString("BLE PAIRING...", 4, 18, true, true);
        drawString("Place device in pair mode", 2, 38, true, false);
    } else {
        drawString("DISCONNECTED", 4, 18, true, true);
        drawString("Press button to pair", 2, 38, true, false);
    }

    // 3. Footer Divider and Instructions (50..63px)
    line(0, 52, OLED_WIDTH - 1, 52, true);
    drawString("[Btn] Press: Pair | 8s: Reset", 2, 55, true, false);

    show();
}

void SSD1306::renderBootSplash(const char *board_desc, const char *version_desc) {
    clear(false);

    // Header banner with inverted text
    rect(0, 0, OLED_WIDTH, 11, true, true);
    drawString("PICO BLE MULTIPLEXER", 14, 2, false, false);

    // Prominent title
    drawString("BLE MULTIPLEX", 12, 16, true, true);

    // Hardware target and firmware version
    if (board_desc) {
        drawString(board_desc, 6, 36, true, false);
    }
    if (version_desc) {
        drawString(version_desc, 6, 46, true, false);
    }

    // Status footer
    line(0, 54, OLED_WIDTH - 1, 54, true);
    drawString("System Ready", 6, 56, true, false);

    show();
}
