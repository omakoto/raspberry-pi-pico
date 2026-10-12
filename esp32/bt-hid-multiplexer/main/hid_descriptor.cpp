#include "hid_descriptor.h"
#include <string.h>

namespace {

// Item tags (HID 1.11, 6.2.2).
enum : uint8_t {
    TYPE_MAIN = 0, TYPE_GLOBAL = 1, TYPE_LOCAL = 2,

    MAIN_INPUT = 8, MAIN_OUTPUT = 9, MAIN_FEATURE = 11,

    GLOBAL_USAGE_PAGE = 0, GLOBAL_REPORT_SIZE = 7, GLOBAL_REPORT_ID = 8, GLOBAL_REPORT_COUNT = 9,
    GLOBAL_PUSH = 10, GLOBAL_POP = 11,

    LOCAL_USAGE = 0, LOCAL_USAGE_MINIMUM = 1,
};

// Input/Output item flags.
enum : uint32_t { FLAG_CONSTANT = 0x01, FLAG_VARIABLE = 0x02, FLAG_RELATIVE = 0x04 };

enum : uint32_t { PAGE_GENERIC_DESKTOP = 0x01, PAGE_KEYBOARD = 0x07, PAGE_LEDS = 0x08, PAGE_CONSUMER = 0x0C, PAGE_DIGITIZER = 0x0D };
enum : uint32_t { USAGE_X = 0x30, USAGE_Y = 0x31, USAGE_LEFT_CONTROL = 0xE0, USAGE_INPUT_MODE = 0x52 };

struct GlobalState {
    uint32_t usage_page;
    uint32_t report_size;
    uint32_t report_count;
    uint8_t report_id;
};

// The bit offset of the next Input (or, in a second table, Feature) field of each report ID seen so far. Descriptors may interleave
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
    InputOffsets feature_offsets;

    // Local items, which apply to the next main item only.
    bool usage_xy = false;
    bool usage_input_mode = false;
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
                if (state.usage_page == PAGE_DIGITIZER && usage == USAGE_INPUT_MODE) {
                    usage_input_mode = true;
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

                if (state.usage_page == PAGE_CONSUMER && data_field && !(val & FLAG_VARIABLE) && bit >= 0 &&
                    bit % 8 == 0 && bit / 8 <= 255 && (state.report_size == 8 || state.report_size == 16) &&
                    state.report_count > 0 && state.report_count <= 255 && !out->has_consumer) {
                    out->has_consumer = true;
                    out->consumer.report_id = state.report_id;
                    out->consumer.offset = (uint8_t)(bit / 8);
                    out->consumer.size_bits = (uint8_t)state.report_size;
                    out->consumer.count = (uint8_t)state.report_count;
                }

                offsets.advance(state.report_id, state.report_size * state.report_count);
            } else if (tag == MAIN_FEATURE) {
                if (usage_input_mode && !out->has_input_mode && state.report_id != 0 &&
                    feature_offsets.get(state.report_id) == 0 && state.report_size == 8) {
                    out->has_input_mode = true;
                    out->input_mode_report_id = state.report_id;
                }
                feature_offsets.advance(state.report_id, state.report_size * state.report_count);
            }
            usage_xy = false;
            usage_input_mode = false;
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

int hid_consumer_report_decode(const HidConsumerLayout &layout, const uint8_t *data, uint16_t data_len,
                               uint16_t *usages, uint8_t max_usages) {
    uint8_t bytes = layout.size_bits / 8;
    if ((uint32_t)layout.offset + (uint32_t)layout.count * bytes > data_len) return -1;
    int n = 0;
    for (uint8_t i = 0; i < layout.count && n < max_usages; i++) {
        const uint8_t *p = data + layout.offset + i * bytes;
        uint16_t usage = (bytes == 2) ? (uint16_t)(p[0] | (p[1] << 8)) : p[0];
        if (usage != 0) usages[n++] = usage;
    }
    return n;
}

namespace {

// Reads `size` bits (up to 32) at bit offset `bit` of a little-endian report.
uint32_t read_bits(const uint8_t *data, uint32_t bit, uint8_t size) {
    uint32_t v = 0;
    for (uint8_t i = 0; i < size; i++) {
        uint32_t b = bit + i;
        if (data[b / 8] & (1u << (b % 8))) v |= 1u << i;
    }
    return v;
}

// Units per millimetre of an axis with logical range [0, logical_max] covering [0, physical_max] in
// `unit` (low nibble: the system, 1 = SI linear in cm, 3 = English linear in inches; next nibble: the
// power of length, which must be 1) times 10^exponent. 0 if the unit is not a length.
float per_mm(int32_t logical_max, int32_t physical_max, uint32_t unit, int32_t exponent) {
    if (logical_max <= 0 || physical_max <= 0 || ((unit >> 4) & 0x0F) != 1) return 0;
    uint32_t system = unit & 0x0F;
    if (system != 1 && system != 3) return 0;
    float mm = (float)physical_max * (system == 1 ? 10.0f : 25.4f);
    for (int32_t e = exponent; e < 0; e++) mm /= 10;
    for (int32_t e = exponent; e > 0; e--) mm *= 10;
    return (float)logical_max / mm;
}

int32_t sign_extend(uint32_t v, uint8_t size) {
    if (size == 0 || size >= 32) return (int32_t)v;
    uint32_t sign = 1u << (size - 1);
    return (int32_t)((v ^ sign) - sign);
}

}  // namespace

bool hid_ptp_parse(const uint8_t *desc, uint16_t desc_len, HidPtpLayout *out) {
    memset(out, 0, sizeof(*out));
    if (!desc) return false;

    struct Globals {
        uint32_t usage_page;
        int32_t logical_max, physical_max;
        uint32_t unit;
        int32_t unit_exponent;
        uint32_t report_size, report_count;
        uint8_t report_id;
    };
    Globals g = {};
    Globals stack[4];
    uint8_t depth = 0;

    // Local usages of the next main item, with their pages.
    uint32_t usages[16];
    uint8_t usage_count = 0;
    uint32_t usage_min = 0, usage_max = 0;
    bool have_range = false;

    // Collections: whether each open one is a Touch Pad application or a Finger.
    enum : uint8_t { COL_OTHER, COL_TOUCHPAD, COL_FINGER };
    uint8_t collections[8];
    uint8_t col_depth = 0;
    bool in_touchpad = false;
    bool found_report = false;
    int8_t finger = -1;  // the finger slot being filled
    bool finger_fields[4];  // tip, id, x, y seen in the current finger

    InputOffsets offsets;
    float x_per_mm = 0, y_per_mm = 0;

    uint16_t i = 0;
    while (i < desc_len) {
        uint8_t b = desc[i++];
        if (b == 0xFE) {
            if (i + 2 > desc_len) break;
            i += 2 + desc[i];
            continue;
        }
        uint8_t tag = (b >> 4) & 0x0F, type = (b >> 2) & 0x03, size = b & 0x03;
        if (size == 3) size = 4;
        if (i + size > desc_len) break;
        uint32_t val = 0;
        for (uint8_t j = 0; j < size; j++) val |= ((uint32_t)desc[i + j]) << (8 * j);
        int32_t sval = (size == 0) ? 0 : sign_extend(val, (uint8_t)(size * 8));
        i += size;

        if (type == TYPE_GLOBAL) {
            switch (tag) {
                case GLOBAL_USAGE_PAGE: g.usage_page = val; break;
                case 2: g.logical_max = (size == 4 || sval >= 0) ? sval : (int32_t)val; break;  // Logical Maximum
                case 4: g.physical_max = (size == 4 || sval >= 0) ? sval : (int32_t)val; break;  // Physical Maximum
                case 5: g.unit_exponent = sign_extend(val & 0x0F, 4); break;  // Unit Exponent
                case 6: g.unit = val; break;  // Unit
                case GLOBAL_REPORT_SIZE: g.report_size = val; break;
                case GLOBAL_REPORT_ID: g.report_id = (uint8_t)val; break;
                case GLOBAL_REPORT_COUNT: g.report_count = val; break;
                case GLOBAL_PUSH: if (depth < 4) stack[depth++] = g; break;
                case GLOBAL_POP: if (depth > 0) g = stack[--depth]; break;
                default: break;
            }
            continue;
        }
        if (type == TYPE_LOCAL) {
            uint32_t full = (size == 4) ? val : ((g.usage_page << 16) | val);
            if (tag == LOCAL_USAGE) {
                if (usage_count < 16) usages[usage_count++] = full;
            } else if (tag == LOCAL_USAGE_MINIMUM) {
                usage_min = full;
                have_range = true;
            } else if (tag == 2) {  // Usage Maximum
                usage_max = full;
            }
            continue;
        }
        if (type != TYPE_MAIN) continue;

        if (tag == 10) {  // Collection
            uint32_t usage = usage_count ? usages[0] : 0;
            uint8_t kind = COL_OTHER;
            if (usage == 0x000D0005 && val == 1) kind = COL_TOUCHPAD;
            if (usage == 0x000D0022 && in_touchpad) kind = COL_FINGER;
            if (col_depth < 8) collections[col_depth] = kind;
            col_depth++;
            if (kind == COL_TOUCHPAD && !found_report) in_touchpad = true;
            if (kind == COL_FINGER) {
                finger = (out->contact_slots < HID_PTP_MAX_CONTACTS) ? (int8_t)out->contact_slots : -1;
                memset(finger_fields, 0, sizeof(finger_fields));
                if (finger >= 0) out->contacts[finger].confidence_bit = -1;
            }
        } else if (tag == 12) {  // End Collection
            if (col_depth > 0) {
                col_depth--;
                uint8_t kind = (col_depth < 8) ? collections[col_depth] : (uint8_t)COL_OTHER;
                if (kind == COL_FINGER && finger >= 0) {
                    if (finger_fields[0] && finger_fields[1] && finger_fields[2] && finger_fields[3]) {
                        out->contact_slots++;
                        found_report = true;
                    }
                    finger = -1;
                } else if (kind == COL_TOUCHPAD && in_touchpad) {
                    in_touchpad = false;
                }
            }
        } else if (tag == MAIN_INPUT) {
            int32_t base = offsets.get(g.report_id);
            bool in_report = in_touchpad && (!found_report || g.report_id == out->report_id) && base >= 0;
            if (in_report && !(val & FLAG_CONSTANT)) {
                if (out->contact_slots == 0 && finger >= 0) out->report_id = g.report_id;
                for (uint32_t k = 0; k < g.report_count; k++) {
                    uint32_t usage;
                    if (usage_count > 0) {
                        usage = usages[(k < usage_count) ? k : usage_count - 1];
                    } else if (have_range) {
                        usage = usage_min + k;
                        if (usage > usage_max && usage_max != 0) usage = usage_max;
                    } else {
                        break;
                    }
                    uint32_t bit = (uint32_t)base + k * g.report_size;
                    if (bit > 0xFFFF) break;
                    uint8_t bits = (uint8_t)g.report_size;
                    if (finger >= 0) {
                        HidPtpContactLayout &c = out->contacts[finger];
                        if (usage == 0x000D0042) { c.tip_bit = (uint16_t)bit; finger_fields[0] = true; }
                        else if (usage == 0x000D0047) { c.confidence_bit = (int16_t)bit; }
                        else if (usage == 0x000D0051) { c.id_bit = (uint16_t)bit; c.id_size = bits; finger_fields[1] = true; }
                        else if (usage == 0x00010030) {
                            c.x_bit = (uint16_t)bit; c.x_size = bits; finger_fields[2] = true;
                            x_per_mm = per_mm(g.logical_max, g.physical_max, g.unit, g.unit_exponent);
                        } else if (usage == 0x00010031) {
                            c.y_bit = (uint16_t)bit; c.y_size = bits; finger_fields[3] = true;
                            y_per_mm = per_mm(g.logical_max, g.physical_max, g.unit, g.unit_exponent);
                        }
                    } else if (usage == 0x000D0054 && !out->has_contact_count) {
                        out->has_contact_count = true;
                        out->contact_count_bit = (uint16_t)bit;
                        out->contact_count_size = bits;
                    } else if (usage == 0x00090001 && !out->has_button) {
                        out->has_button = true;
                        out->button_bit = (uint16_t)bit;
                    }
                }
            }
            if (base >= 0) offsets.advance(g.report_id, g.report_size * g.report_count);
        }
        usage_count = 0;
        have_range = false;
        usage_min = usage_max = 0;
    }

    if (out->contact_slots == 0) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    out->x_per_mm = x_per_mm;
    out->y_per_mm = y_per_mm;
    return true;
}

bool hid_ptp_report_decode(const HidPtpLayout &layout, const uint8_t *data, uint16_t data_len,
                           HidPtpContact contacts[HID_PTP_MAX_CONTACTS], uint8_t *contact_count, bool *button) {
    // The report must hold every field.
    uint32_t end = 0;
    auto need = [&end](uint32_t bit, uint32_t size) { if (bit + size > end) end = bit + size; };
    for (uint8_t f = 0; f < layout.contact_slots; f++) {
        const HidPtpContactLayout &c = layout.contacts[f];
        need(c.tip_bit, 1);
        if (c.confidence_bit >= 0) need((uint32_t)c.confidence_bit, 1);
        need(c.id_bit, c.id_size);
        need(c.x_bit, c.x_size);
        need(c.y_bit, c.y_size);
    }
    if (layout.has_contact_count) need(layout.contact_count_bit, layout.contact_count_size);
    if (layout.has_button) need(layout.button_bit, 1);
    if ((end + 7) / 8 > data_len) return false;

    for (uint8_t f = 0; f < layout.contact_slots; f++) {
        const HidPtpContactLayout &c = layout.contacts[f];
        contacts[f].tip = read_bits(data, c.tip_bit, 1) != 0;
        contacts[f].confident = c.confidence_bit < 0 || read_bits(data, (uint32_t)c.confidence_bit, 1) != 0;
        contacts[f].id = (uint8_t)read_bits(data, c.id_bit, c.id_size);
        contacts[f].x = (int32_t)read_bits(data, c.x_bit, c.x_size);
        contacts[f].y = (int32_t)read_bits(data, c.y_bit, c.y_size);
    }
    *contact_count = layout.has_contact_count
                         ? (uint8_t)read_bits(data, layout.contact_count_bit, layout.contact_count_size)
                         : layout.contact_slots;
    *button = layout.has_button && read_bits(data, layout.button_bit, 1) != 0;
    return true;
}
