#include "status_led.h"

#include <stdio.h>

#include "config.h"

#if defined(BOARD_XIAO)

#include "driver/gpio.h"

void status_led_init() {
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << PIN_LED;
    io.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io);
    status_led_set(false);
}

void status_led_set(bool on) {
    gpio_set_level((gpio_num_t)PIN_LED, on ? 0 : 1);  // active LOW
}

#else  // BOARD_DEVKITC

#include "led_strip.h"

// "On" colour of the WS2812: dim blue, enough to see without glaring.
static const uint8_t LED_ON_R = 0, LED_ON_G = 0, LED_ON_B = 24;

// The WS2812 is on GPIO48 on DevKitC v1.0 boards and on GPIO38 on v1.1 boards. Both pins are driven
// with the same data, so the LED works on either revision; on the other pin the signal goes nowhere.
static led_strip_handle_t s_strips[2];

static led_strip_handle_t new_strip(int gpio) {
    led_strip_config_t strip_cfg = {};
    strip_cfg.strip_gpio_num = gpio;
    strip_cfg.max_leds = 1;
    strip_cfg.led_model = LED_MODEL_WS2812;
    strip_cfg.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    led_strip_rmt_config_t rmt_cfg = {};
    rmt_cfg.clk_src = RMT_CLK_SRC_DEFAULT;
    rmt_cfg.resolution_hz = 10 * 1000 * 1000;
    led_strip_handle_t strip = nullptr;
    if (led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &strip) != ESP_OK) {
        printf("[LED] Could not drive the RGB LED on GPIO%d\n", gpio);
        return nullptr;
    }
    return strip;
}

void status_led_init() {
    s_strips[0] = new_strip(PIN_RGB_LED_V1_0);
    s_strips[1] = new_strip(PIN_RGB_LED_V1_1);
    status_led_set(false);
}

void status_led_set(bool on) {
    for (led_strip_handle_t strip : s_strips) {
        if (!strip) continue;
        if (on) {
            led_strip_set_pixel(strip, 0, LED_ON_R, LED_ON_G, LED_ON_B);
            led_strip_refresh(strip);
        } else {
            led_strip_clear(strip);
        }
    }
}

#endif
