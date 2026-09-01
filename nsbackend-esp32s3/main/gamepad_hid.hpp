#pragma once

#include <cstdint>
#include <cstddef>
#include "esp_err.h"

// Nintendo Switch Button Masks (16-bit uint16 little-endian)
constexpr uint16_t BTN_NONE    = 0x0000;
constexpr uint16_t BTN_Y       = 1 << 0;
constexpr uint16_t BTN_B       = 1 << 1;
constexpr uint16_t BTN_A       = 1 << 2;
constexpr uint16_t BTN_X       = 1 << 3;
constexpr uint16_t BTN_L       = 1 << 4;
constexpr uint16_t BTN_R       = 1 << 5;
constexpr uint16_t BTN_ZL      = 1 << 6;
constexpr uint16_t BTN_ZR      = 1 << 7;
constexpr uint16_t BTN_MINUS   = 1 << 8;
constexpr uint16_t BTN_PLUS    = 1 << 9;
constexpr uint16_t BTN_LSTICK  = 1 << 10;
constexpr uint16_t BTN_RSTICK  = 1 << 11;
constexpr uint16_t BTN_HOME    = 1 << 12;
constexpr uint16_t BTN_CAPTURE = 1 << 13;

// Hat Switch (D-Pad) Values
constexpr uint8_t HAT_UP         = 0x00;
constexpr uint8_t HAT_UP_RIGHT   = 0x01;
constexpr uint8_t HAT_RIGHT      = 0x02;
constexpr uint8_t HAT_DOWN_RIGHT = 0x03;
constexpr uint8_t HAT_DOWN       = 0x04;
constexpr uint8_t HAT_DOWN_LEFT  = 0x05;
constexpr uint8_t HAT_LEFT       = 0x06;
constexpr uint8_t HAT_UP_LEFT    = 0x07;
constexpr uint8_t HAT_CENTER     = 0x08;

#pragma pack(push, 1)
struct SwitchReport {
    uint16_t buttons;
    uint8_t hat;
    uint8_t lx;
    uint8_t ly;
    uint8_t rx;
    uint8_t ry;
    uint8_t vendor;
};
#pragma pack(pop)

static_assert(sizeof(SwitchReport) == 8, "Switch HID report must be exactly 8 bytes");

class GamepadHid {
public:
    GamepadHid();
    ~GamepadHid();

    bool init();
    bool is_mounted() const;
    void send_report(uint16_t buttons, uint8_t hat, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry);

private:
    SwitchReport last_report_;
    bool initialized_;
};
