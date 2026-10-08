#include "oled.h"
#include "font_8x16.h"
#include "font_4x5.h"
#include "driver/i2c_master.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#if OLED_CONTROLLER != OLED_SSD1306 && OLED_CONTROLLER != OLED_SH1106
#error "OLED_CONTROLLER must be OLED_SSD1306 or OLED_SH1106"
#endif

// The SH1106 has 132 columns of RAM and 128-pixel modules wire the panel to columns 2..129.
#if OLED_CONTROLLER == OLED_SH1106
static const uint8_t OLED_COL_OFFSET = 2;
#else
static const uint8_t OLED_COL_OFFSET = 0;
#endif

uint8_t Oled::buffer_[OLED_WIDTH * OLED_HEIGHT / 8];

// A transfer that takes longer than this means a stuck bus; it is abandoned instead of blocking the
// UI task (which the task watchdog watches).
static const int I2C_TIMEOUT_MS = 100;

static i2c_master_bus_handle_t s_bus = nullptr;
static i2c_master_dev_handle_t s_dev = nullptr;

void Oled::write(const uint8_t *data, size_t len) {
    if (s_dev) i2c_master_transmit(s_dev, data, len, I2C_TIMEOUT_MS);
}

void Oled::writeCmd(uint8_t cmd) {
    uint8_t payload[2] = {0x00, cmd};
    write(payload, 2);
}

void Oled::writeCmdList(const uint8_t *cmds, size_t len) {
    uint8_t payload[32];
    payload[0] = 0x00;
    while (len > 0) {
        size_t chunk = (len > 31) ? 31 : len;
        memcpy(&payload[1], cmds, chunk);
        write(payload, chunk + 1);
        cmds += chunk;
        len -= chunk;
    }
}

bool Oled::init() {
    // I2C0 master with the internal pull-ups (most OLED modules have their own as well).
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = I2C_NUM_0;
    bus_cfg.sda_io_num = (gpio_num_t)PIN_OLED_SDA;
    bus_cfg.scl_io_num = (gpio_num_t)PIN_OLED_SCL;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;
    if (i2c_new_master_bus(&bus_cfg, &s_bus) != ESP_OK) {
        printf("[OLED] I2C bus init failed\n");
        return false;
    }
    if (i2c_master_probe(s_bus, OLED_I2C_ADDR, I2C_TIMEOUT_MS) != ESP_OK) {
        printf("[OLED] No display at 0x%02X (SDA GPIO%d, SCL GPIO%d); running without it\n",
               OLED_I2C_ADDR, PIN_OLED_SDA, PIN_OLED_SCL);
        return false;
    }
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = OLED_I2C_ADDR;
    dev_cfg.scl_speed_hz = OLED_BAUDRATE_HZ;
    if (i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev) != ESP_OK) {
        printf("[OLED] I2C device init failed\n");
        return false;
    }

    // 128x64 initialization sequence. The two controllers share most commands; they differ in how
    // the panel voltage is generated and in the addressing modes. show() uses page addressing, the
    // only mode the SH1106 has. The SSD1306 keeps its mode across a reset of the ESP32, so it is set
    // explicitly.
    const uint8_t init_cmds[] = {
        0xAE,        // Display OFF
        0xD5, 0x80,  // Set display clock divide ratio / oscillator frequency
        0xA8, 0x3F,  // Set multiplex ratio (1 to 64)
        0xD3, 0x00,  // Set display offset to 0
        0x40,        // Set display start line to 0
#if OLED_CONTROLLER == OLED_SH1106
        0xAD, 0x8B,  // Enable the DC-DC converter
#else
        0x8D, 0x14,  // Enable charge pump regulator
        0x20, 0x02,  // Set memory addressing mode to Page
#endif
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
    return true;
}

int Oled::scanBus(uint8_t *found, int max) {
    if (!s_bus) return -1;
    int count = 0;
    for (uint8_t addr = 0x08; addr <= 0x77 && count < max; addr++) {
        if (i2c_master_probe(s_bus, addr, 20) == ESP_OK) found[count++] = addr;
    }
    return count;
}

void Oled::clear(bool color) {
    memset(buffer_, color ? 0xFF : 0x00, sizeof(buffer_));
}

void Oled::pixel(int x, int y, bool color) {
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

void Oled::line(int x0, int y0, int x1, int y1, bool color) {
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

void Oled::rect(int x, int y, int w, int h, bool color, bool fill) {
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

int Oled::drawChar(char c, int x, int y, bool color, bool font_large) {
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

void Oled::drawString(const char *s, int x, int y, bool color, bool font_large) {
    if (!s) return;
    int curr_x = x;
    while (*s) {
        int width = drawChar(*s, curr_x, y, color, font_large);
        curr_x += width;
        s++;
    }
}

void Oled::show() {
    // One page (8 pixel rows) at a time in page addressing mode, the mode both controllers support.
    // Each page is a command transfer that sets the page and start column, then a data transfer
    // prefixed with the data indicator 0x40 (Co=0, D/C#=1).
    uint8_t chunk_payload[OLED_WIDTH + 1];
    chunk_payload[0] = 0x40;

    for (int page = 0; page < OLED_HEIGHT / 8; page++) {
        const uint8_t addr_cmd[] = {
            (uint8_t)(0xB0 | page),                   // Set page address
            (uint8_t)(0x00 | (OLED_COL_OFFSET & 0x0F)), // Set lower column address
            (uint8_t)(0x10 | (OLED_COL_OFFSET >> 4)),   // Set higher column address
        };
        writeCmdList(addr_cmd, sizeof(addr_cmd));
        memcpy(&chunk_payload[1], &buffer_[page * OLED_WIDTH], OLED_WIDTH);
        write(chunk_payload, sizeof(chunk_payload));
    }
}

void Oled::renderStatus(uint8_t connected_count, const char *dev_name, int active_layer,
                           bool pairing_active, uint32_t passkey, const char *toast_msg, bool usb_mounted) {
    bool ble_connected = connected_count > 0;
    clear(false);

    // 1. Header Banner (0..11px)
    rect(0, 0, OLED_WIDTH, 11, true, true);
    if (pairing_active && ble_connected) {
        drawString("MULTIPLEXER [PAIR]", 4, 2, false, false);
    } else {
        drawString("ESP32 MULTIPLEXER", 4, 2, false, false);
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
        // "<number of connected devices> devs: <name of the device used last>"
        char dev_line[48];
        snprintf(dev_line, sizeof(dev_line), "%u devs: %s", connected_count, dev_name ? dev_name : "");
        drawString(dev_line, 2, 16, true, false);

        char layer_str[32];
        const char *l_name = "BASE";
        if (active_layer == 1) l_name = "NAV/MEDIA";
        else if (active_layer == 2) l_name = "FUNCTION";
        else if (active_layer == 3) l_name = "NUMPAD";
        else if (active_layer > 3) l_name = "";
        snprintf(layer_str, sizeof(layer_str), l_name[0] ? "LAYER %d: %s" : "LAYER %d", active_layer, l_name);
        drawString(layer_str, 2, 28, true, false);

        if (toast_msg && toast_msg[0] != '\0') {
            drawString(toast_msg, 2, 40, true, false);
        } else if (pairing_active) {
            drawString("Pairing mode (60s)... | USB", 2, 40, true, false);
        } else if (usb_mounted) {
            drawString("USB: Active | VIAL: Ready", 2, 40, true, false);
        } else {
            drawString("USB: Not connected", 2, 40, true, false);
        }
    } else if (pairing_active) {
        drawString("BT PAIRING...", 4, 18, true, true);
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

void Oled::renderBootSplash(const char *board_desc, const char *version_desc) {
    clear(false);

    // Header banner with inverted text
    rect(0, 0, OLED_WIDTH, 11, true, true);
    drawString("ESP32 BT MULTIPLEXER", 14, 2, false, false);

    // Prominent title
    drawString("BT MULTIPLEX", 16, 16, true, true);

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
