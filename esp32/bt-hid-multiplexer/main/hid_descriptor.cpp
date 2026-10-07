#include "hid_descriptor.h"
#include <string.h>

namespace {

// Item tags (HID 1.11, 6.2.2).
enum : uint8_t {
    TYPE_MAIN = 0, TYPE_GLOBAL = 1, TYPE_LOCAL = 2,

    MAIN_INPUT = 8, MAIN_OUTPUT = 9,

    GLOBAL_USAGE_PAGE = 0, GLOBAL_REPORT_SIZE = 7, GLOBAL_REPORT_ID = 8, GLOBAL_REPORT_COUNT = 9,
    GLOBAL_PUSH = 10, GLOBAL_POP = 11,

    LOCAL_USAGE = 0, LOCAL_USAGE_MINIMUM = 1,
};

// Input/Output item flags.
enum : uint32_t { FLAG_CONSTANT = 0x01, FLAG_VARIABLE = 0x02, FLAG_RELATIVE = 0x04 };

enum : uint32_t { PAGE_GENERIC_DESKTOP = 0x01, PAGE_KEYBOARD = 0x07, PAGE_LEDS = 0x08 };
enum : uint32_t { USAGE_X = 0x30, USAGE_Y = 0x31, USAGE_LEFT_CONTROL = 0xE0 };

struct GlobalState {
    uint32_t usage_page;
    uint32_t report_size;
    uint32_t report_count;
    uint8_t report_id;
};

// The bit offset of the next Input field of each report ID seen so far. Descriptors may interleave
// report IDs, so each keeps its own. Peripherals use a handful of IDs; offsets in IDs beyond the
// table are unknown, and a keyboard found there is not used.
class InputOffsets {
public:
    // Returns the offset of report `id`, or -1 if it does not fit in the table.
    int32_t get(uint8_t id) {
        Entry *e = find(id);
        return e ? (int32_t)e->bits : -1;
    }

    void advance(uint8_t id, uint32_t bits) {
        Entry *e = find(id);
        if (e) e->bits += bits;
    }

private:
    struct Entry {
        uint8_t id;
        uint32_t bits;
    };
    static const uint8_t MAX_IDS = 16;

    Entry *find(uint8_t id) {
        for (uint8_t i = 0; i < count_; i++) {
            if (entries_[i].id == id) return &entries_[i];
        }
        if (count_ == MAX_IDS) return nullptr;
        entries_[count_] = {id, 0};
        return &entries_[count_++];
    }

    Entry entries_[MAX_IDS];
    uint8_t count_ = 0;
};

}  // namespace

void hid_descriptor_parse(const uint8_t *desc, uint16_t desc_len, HidDescriptorInfo *out) {
    memset(out, 0, sizeof(*out));
    if (!desc) return;

    GlobalState state = {};
    GlobalState stack[4];
    uint8_t stack_depth = 0;
    InputOffsets offsets;

    // Local items, which apply to the next main item only.
    bool usage_xy = false;
    uint32_t usage_minimum = 0;

    // The modifier bits seen last, to pair with a keycode array in the same report.
    bool have_modifiers = false;
    uint8_t modifiers_id = 0;
    int32_t modifiers_bit = 0;

    bool have_absolute_mouse = false;
    uint8_t absolute_mouse_id = 0;
    uint8_t absolute_mouse_bits = 0;

    uint16_t i = 0;
    while (i < desc_len) {
        uint8_t b = desc[i++];
        if (b == 0xFE) {  // Long item: 0xFE, bDataSize, bLongItemTag, data...
            if (i + 2 > desc_len) break;
            i += 2 + desc[i];
            continue;
        }

        uint8_t tag = (b >> 4) & 0x0F;
        uint8_t type = (b >> 2) & 0x03;
        uint8_t size = b & 0x03;
        if (size == 3) size = 4;
        if (i + size > desc_len) break;

        uint32_t val = 0;
        for (uint8_t j = 0; j < size; j++) {
            val |= ((uint32_t)desc[i + j]) << (8 * j);
        }
        i += size;

        if (type == TYPE_GLOBAL) {
            if (tag == GLOBAL_USAGE_PAGE) {
                state.usage_page = val;
            } else if (tag == GLOBAL_REPORT_SIZE) {
                state.report_size = val;
            } else if (tag == GLOBAL_REPORT_ID) {
                state.report_id = (uint8_t)val;
            } else if (tag == GLOBAL_REPORT_COUNT) {
                state.report_count = val;
            } else if (tag == GLOBAL_PUSH) {
                if (stack_depth < 4) stack[stack_depth++] = state;
            } else if (tag == GLOBAL_POP) {
                if (stack_depth > 0) state = stack[--stack_depth];
            }
        } else if (type == TYPE_LOCAL) {
            // A 4-byte usage carries its own usage page in the upper half; only the usage is kept,
            // which is what devices that use this form put there in practice.
            uint32_t usage = (size == 4) ? (val & 0xFFFF) : val;
            if (tag == LOCAL_USAGE) {
                if (state.usage_page == PAGE_GENERIC_DESKTOP && (usage == USAGE_X || usage == USAGE_Y)) {
                    usage_xy = true;
                }
            } else if (tag == LOCAL_USAGE_MINIMUM) {
                usage_minimum = usage;
            }
        } else if (type == TYPE_MAIN) {
            if (tag == MAIN_OUTPUT && state.usage_page == PAGE_LEDS && !out->has_led_output) {
                out->has_led_output = true;
                out->led_report_id = state.report_id;
            } else if (tag == MAIN_INPUT) {
                int32_t bit = offsets.get(state.report_id);
                bool data_field = !(val & FLAG_CONSTANT);

                if (usage_xy && state.usage_page == PAGE_GENERIC_DESKTOP) {
                    if (val & FLAG_RELATIVE) {
                        if (!out->has_mouse) {
                            out->has_mouse = true;
                            out->mouse_report_id = state.report_id;
                            out->mouse_xy_bits = (uint8_t)state.report_size;
                        }
                    } else if (!have_absolute_mouse) {
                        have_absolute_mouse = true;
                        absolute_mouse_id = state.report_id;
                        absolute_mouse_bits = (uint8_t)state.report_size;
                    }
                }

                if (state.usage_page == PAGE_KEYBOARD && data_field && bit >= 0 && bit % 8 == 0) {
                    if ((val & FLAG_VARIABLE) && state.report_size == 1 && state.report_count == 8 &&
                        usage_minimum == USAGE_LEFT_CONTROL) {
                        have_modifiers = true;
                        modifiers_id = state.report_id;
                        modifiers_bit = bit;
                    } else if (!(val & FLAG_VARIABLE) && state.report_size == 8 && state.report_count > 0 &&
                               !out->has_keyboard && bit / 8 + state.report_count <= 255) {
                        out->has_keyboard = true;
                        out->keyboard.report_id = state.report_id;
                        out->keyboard.modifier_offset =
                            (have_modifiers && modifiers_id == state.report_id && modifiers_bit / 8 <= 127)
                                ? (int8_t)(modifiers_bit / 8) : -1;
                        out->keyboard.keys_offset = (uint8_t)(bit / 8);
                        out->keyboard.key_count = (uint8_t)state.report_count;
                    }
                }

                offsets.advance(state.report_id, state.report_size * state.report_count);
            }
            usage_xy = false;
            usage_minimum = 0;
        }
    }

    if (!out->has_mouse && have_absolute_mouse) {
        out->has_mouse = true;
        out->mouse_report_id = absolute_mouse_id;
        out->mouse_xy_bits = absolute_mouse_bits;
        out->mouse_absolute = true;
    }
}

bool hid_keyboard_report_decode(const HidKeyboardLayout &layout, const uint8_t *data, uint16_t data_len,
                                uint8_t *modifiers, uint8_t keys[6]) {
    if (layout.modifier_offset >= data_len || layout.keys_offset + layout.key_count > data_len) {
        return false;
    }
    *modifiers = (layout.modifier_offset >= 0) ? data[layout.modifier_offset] : 0;
    memset(keys, 0, 6);
    uint8_t n = 0;
    for (uint8_t k = 0; k < layout.key_count && n < 6; k++) {
        uint8_t code = data[layout.keys_offset + k];
        if (code != 0) keys[n++] = code;
    }
    return true;
}
