// Bluetooth Classic (BR/EDR) HID host built on BTstack's hid_host. See classic_hid_host.h.
//
// Flow: in pairing mode an inquiry looks for peripheral-class devices (keyboards, keypads, mice)
// and connects to the first one found; BTstack's L2CAP security level 2 triggers pairing
// (SSP "just works"/passkey, or the legacy PIN "0000"), after which the link key is kept in the
// flash TLV link key DB. Bonded devices reconnect by themselves because the host is always
// connectable. Incoming HID reports are decoded with a small descriptor parser and fed to the
// Multiplexer exactly like the BLE ones.

#include "classic_hid_host.h"
#include "ble_hid_host.h"
#include "multiplexer.h"
#include "config.h"
#include "btstack.h"
#include "pico/time.h"
#include <stdio.h>
#include <string.h>

// Peripheral major device class (Class of Device bits 8..12).
#define COD_MAJOR_MASK          0x1F00
#define COD_MAJOR_PERIPHERAL    0x0500

// One inquiry burst; restarted while pairing mode lasts so that connects start promptly.
#define INQUIRY_DURATION_1280MS 8

// Legacy (pre-SSP) pairing PIN, which is what simple keypads and old keyboards expect.
#define LEGACY_PIN              "0000"

#define HID_BOOT_KBD_REPORT_LEN 8

// Latency bound requested from the controller for each classic HID link. Without it the CYW43
// polls an idle-looking peripheral rarely enough that a trackpad's reports arrive in bursts of 3-4
// every ~40 ms; with 5 ms they arrive at a steady ~10 ms cadence. Tighter values (2.5 ms) were
// measured to be no better.
#define LINK_QOS_LATENCY_US     5000

// An inter-report gap above this is visible as a stutter on a pointing device.
#define SLOW_GAP_US             30000
// Upper bounds (us) of the gap histogram buckets; the last bucket is open-ended.
static const uint32_t GAP_BUCKET_US[5] = {2000, 8000, 16000, 30000, 60000};

struct FieldLoc {
    bool valid;
    bool is_signed;
    uint8_t size;     // bits
    uint16_t off;     // bit offset from the start of the report body (after the report ID)
};

// What the report descriptor says about the input/output reports we care about.
struct ReportMap {
    bool parsed;
    bool uses_report_ids;
    bool has_kbd;
    uint8_t kbd_id;
    bool has_mouse;
    uint8_t mouse_id;
    FieldLoc buttons, x, y, wheel, pan;
    bool has_led;
    uint8_t led_id;
};

struct ClassicSlot {
    bool in_use;
    uint16_t hid_cid;
    bd_addr_t addr;
    char name[32];
    uint8_t dev_idx;
    bool report_protocol;   // false: boot protocol (fixed-layout reports, no report IDs)
    bool descriptor_pending; // reports cannot be told apart until the descriptor has been resolved
    ReportMap map;
    uint32_t last_used_ms;
    // Mouse report arrival statistics since the last 'devices' dump (diagnoses motion stutter).
    uint32_t stat_start_us;
    uint32_t stat_last_us;
    uint32_t stat_count;
    uint32_t stat_max_gap_us;
    uint32_t stat_slow_gaps;   // gaps above SLOW_GAP_US
    uint32_t stat_hist[6];     // gap histogram, see GAP_BUCKET_US
    hci_con_handle_t con_handle;
};

static ClassicSlot s_slots[MAX_CLASSIC_DEVICES];
static uint8_t s_hid_descriptor_storage[2048];

static bool s_pairing = false;
static bool s_inquiry_active = false;
static bool s_connecting = false;
static bd_addr_t s_connecting_addr;
static char s_connecting_name[32];
static uint32_t s_passkey = 0;

static btstack_packet_callback_registration_t s_hci_registration;

static void handle_hci_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void handle_hid_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

// ---------------------------------------------------------------------------------------------
// Slot helpers
// ---------------------------------------------------------------------------------------------

static ClassicSlot* find_slot_by_cid(uint16_t cid) {
    for (uint8_t i = 0; i < MAX_CLASSIC_DEVICES; i++) {
        if (s_slots[i].in_use && s_slots[i].hid_cid == cid) return &s_slots[i];
    }
    return nullptr;
}

static ClassicSlot* find_slot_by_addr(const bd_addr_t addr) {
    for (uint8_t i = 0; i < MAX_CLASSIC_DEVICES; i++) {
        if (s_slots[i].in_use && bd_addr_cmp(s_slots[i].addr, addr) == 0) return &s_slots[i];
    }
    return nullptr;
}

static ClassicSlot* alloc_slot() {
    for (uint8_t i = 0; i < MAX_CLASSIC_DEVICES; i++) {
        if (!s_slots[i].in_use) {
            memset(&s_slots[i], 0, sizeof(ClassicSlot));
            s_slots[i].in_use = true;
            s_slots[i].dev_idx = MAX_BLE_DEVICES + i;
            s_slots[i].report_protocol = true;
            s_slots[i].descriptor_pending = true;
            return &s_slots[i];
        }
    }
    return nullptr;
}

static void free_slot(ClassicSlot *slot) {
    Multiplexer::purgeKeyboard(slot->dev_idx);
    Multiplexer::purgeMouse(slot->dev_idx);
    memset(slot, 0, sizeof(ClassicSlot));
}

static bool has_free_slot() {
    for (uint8_t i = 0; i < MAX_CLASSIC_DEVICES; i++) {
        if (!s_slots[i].in_use) return true;
    }
    return false;
}

static bool is_bonded(const bd_addr_t addr) {
    btstack_link_key_iterator_t it;
    if (!gap_link_key_iterator_init(&it)) return false;
    bd_addr_t a;
    link_key_t key;
    link_key_type_t type;
    bool found = false;
    while (gap_link_key_iterator_get_next(&it, a, key, &type)) {
        if (bd_addr_cmp(a, addr) == 0) {
            found = true;
            break;
        }
    }
    gap_link_key_iterator_done(&it);
    return found;
}

// ---------------------------------------------------------------------------------------------
// HID report descriptor parsing
// ---------------------------------------------------------------------------------------------

#define USAGE_PAGE_GENERIC_DESKTOP 0x01
#define USAGE_PAGE_LED             0x08
#define USAGE_PAGE_BUTTON          0x09
#define USAGE_PAGE_CONSUMER        0x0C
#define USAGE_POINTER_APP          ((USAGE_PAGE_GENERIC_DESKTOP << 16) | 0x02) // Mouse
#define USAGE_KEYBOARD_APP         ((USAGE_PAGE_GENERIC_DESKTOP << 16) | 0x06)
#define USAGE_KEYPAD_APP           ((USAGE_PAGE_GENERIC_DESKTOP << 16) | 0x07)
#define USAGE_AC_PAN               0x0238

struct ReportBits {
    uint8_t id;
    uint16_t in_bits;
    uint16_t out_bits;
};

static ReportBits* bits_for(ReportBits *tbl, uint8_t *n, uint8_t id) {
    for (uint8_t i = 0; i < *n; i++) {
        if (tbl[i].id == id) return &tbl[i];
    }
    if (*n >= 8) return &tbl[7];
    tbl[*n] = ReportBits{id, 0, 0};
    return &tbl[(*n)++];
}

static void set_field(FieldLoc *f, uint16_t off, uint8_t size, bool is_signed) {
    if (f->valid) return;
    f->valid = true;
    f->off = off;
    f->size = size;
    f->is_signed = is_signed;
}

// Extracts what the multiplexer needs: the keyboard report (boot-compatible layout is assumed),
// the mouse button/axis bit positions (any width, e.g. Logitech 12-bit axes) and the LED output
// report ID.
static void parse_report_map(const uint8_t *d, uint16_t len, ReportMap *m) {
    memset(m, 0, sizeof(*m));

    uint32_t usage_page = 0;
    uint8_t report_size = 0;
    uint16_t report_count = 0;
    uint8_t report_id = 0;
    int32_t logical_min = 0;
    uint16_t usages[16];
    uint8_t n_usages = 0;
    uint16_t usage_min = 0, usage_max = 0;
    uint32_t app = 0;
    int depth = 0;
    ReportBits tbl[8];
    uint8_t n_tbl = 0;

    uint16_t i = 0;
    while (i < len) {
        uint8_t b = d[i];
        if (b == 0xFE) {   // long item: skip
            if (i + 1 >= len) break;
            i += d[i + 1] + 3;
            continue;
        }
        uint8_t sz = b & 3;
        if (sz == 3) sz = 4;
        uint8_t type = (b >> 2) & 3;
        uint8_t tag = b >> 4;
        if (i + 1 + sz > len) break;
        uint32_t u = 0;
        for (uint8_t k = 0; k < sz; k++) u |= (uint32_t)d[i + 1 + k] << (8 * k);
        int32_t s = (sz == 0) ? 0 : (sz == 4 ? (int32_t)u : (int32_t)(u << (32 - 8 * sz)) >> (32 - 8 * sz));
        i += 1 + sz;

        if (type == 1) {            // Global
            switch (tag) {
                case 0x0: usage_page = u; break;
                case 0x1: logical_min = s; break;
                case 0x7: report_size = (uint8_t)u; break;
                case 0x8: report_id = (uint8_t)u; if (report_id) m->uses_report_ids = true; break;
                case 0x9: report_count = (uint16_t)u; break;
                default: break;
            }
        } else if (type == 2) {     // Local
            switch (tag) {
                case 0x0: if (n_usages < 16) usages[n_usages++] = (uint16_t)u; break;
                case 0x1: usage_min = (uint16_t)u; break;
                case 0x2: usage_max = (uint16_t)u; break;
                default: break;
            }
        } else if (type == 0) {     // Main
            if (tag == 0xA) {       // Collection
                if (depth == 0 && (u & 0xFF) == 1 && n_usages > 0) {
                    app = (usage_page << 16) | usages[0];
                }
                depth++;
            } else if (tag == 0xC) {    // End Collection
                if (depth > 0) depth--;
                if (depth == 0) app = 0;
            } else if (tag == 0x8 || tag == 0x9) {    // Input / Output
                ReportBits *rb = bits_for(tbl, &n_tbl, report_id);
                uint16_t *bits = (tag == 0x8) ? &rb->in_bits : &rb->out_bits;
                bool constant = (u & 1) != 0;
                bool is_signed = logical_min < 0;

                if (tag == 0x8 && !constant) {
                    if (app == USAGE_KEYBOARD_APP || app == USAGE_KEYPAD_APP) {
                        if (!m->has_kbd) {
                            m->has_kbd = true;
                            m->kbd_id = report_id;
                        }
                    } else if (app == USAGE_POINTER_APP) {
                        m->has_mouse = true;
                        m->mouse_id = report_id;
                        if (usage_page == USAGE_PAGE_BUTTON) {
                            set_field(&m->buttons, *bits, (uint8_t)(report_size * report_count), false);
                        } else {
                            for (uint16_t f = 0; f < report_count; f++) {
                                uint16_t usage;
                                if (n_usages > 0) usage = usages[f < n_usages ? f : n_usages - 1];
                                else usage = usage_min + f <= usage_max ? usage_min + f : usage_max;
                                uint16_t off = *bits + f * report_size;
                                if (usage_page == USAGE_PAGE_GENERIC_DESKTOP) {
                                    if (usage == 0x30) set_field(&m->x, off, report_size, is_signed);
                                    else if (usage == 0x31) set_field(&m->y, off, report_size, is_signed);
                                    else if (usage == 0x38) set_field(&m->wheel, off, report_size, is_signed);
                                } else if (usage_page == USAGE_PAGE_CONSUMER && usage == USAGE_AC_PAN) {
                                    set_field(&m->pan, off, report_size, is_signed);
                                }
                            }
                        }
                    }
                } else if (tag == 0x9 && !constant && usage_page == USAGE_PAGE_LED && !m->has_led) {
                    m->has_led = true;
                    m->led_id = report_id;
                }
                *bits += report_size * report_count;
            }
            n_usages = 0;
            usage_min = usage_max = 0;
        }
    }
    m->parsed = true;
}

static int32_t get_field(const uint8_t *d, uint16_t len, const FieldLoc &f) {
    uint32_t v = 0;
    for (uint8_t i = 0; i < f.size && i < 32; i++) {
        uint16_t bit = f.off + i;
        if ((bit >> 3) >= len) break;
        if ((d[bit >> 3] >> (bit & 7)) & 1) v |= 1u << i;
    }
    if (f.is_signed && f.size > 0 && f.size < 32 && ((v >> (f.size - 1)) & 1)) {
        v |= ~0u << f.size;
    }
    return (int32_t)v;
}

template <typename T>
static T clamp_to(int32_t v, int32_t lo, int32_t hi) {
    return (T)(v < lo ? lo : (v > hi ? hi : v));
}

// ---------------------------------------------------------------------------------------------
// Report handling
// ---------------------------------------------------------------------------------------------

static void record_mouse_arrival(ClassicSlot *s) {
    uint32_t now = time_us_32();
    if (s->stat_count == 0) {
        s->stat_start_us = now;
    } else {
        uint32_t gap = now - s->stat_last_us;
        if (gap > s->stat_max_gap_us) s->stat_max_gap_us = gap;
        if (gap > SLOW_GAP_US) s->stat_slow_gaps++;
        uint8_t b = 0;
        while (b < 5 && gap >= GAP_BUCKET_US[b]) b++;
        s->stat_hist[b]++;
    }
    s->stat_last_us = now;
    s->stat_count++;
}

static void handle_mouse(ClassicSlot *s, const uint8_t *d, uint16_t len, const ReportMap &m) {
    record_mouse_arrival(s);
    uint8_t buttons = m.buttons.valid ? (uint8_t)get_field(d, len, m.buttons) : 0;
    int16_t dx = m.x.valid ? clamp_to<int16_t>(get_field(d, len, m.x), -32768, 32767) : 0;
    int16_t dy = m.y.valid ? clamp_to<int16_t>(get_field(d, len, m.y), -32768, 32767) : 0;
    int8_t wheel = m.wheel.valid ? clamp_to<int8_t>(get_field(d, len, m.wheel), -127, 127) : 0;
    int8_t pan = m.pan.valid ? clamp_to<int8_t>(get_field(d, len, m.pan), -127, 127) : 0;
    Multiplexer::handleMouseReport(s->dev_idx, buttons, dx, dy, wheel, pan);
}

static void handle_report(ClassicSlot *s, const uint8_t *rep, uint16_t len) {
    // A peripheral that reconnects by itself may send reports before its descriptor is known. Its
    // report IDs would then be misread as boot-layout data (a mouse report with ID 5 decodes as
    // modifier byte 0x05), leaving modifier keys stuck, so such reports are dropped.
    if (s->descriptor_pending) return;
    // The first byte is the HIDP DATA|INPUT header.
    if (len < 2 || rep[0] != 0xA1) return;
    rep++;
    len--;
    s->last_used_ms = to_ms_since_boot(get_absolute_time());

    const ReportMap &m = s->map;
    if (m.parsed && s->report_protocol) {
        uint8_t id = 0;
        if (m.uses_report_ids) {
            id = rep[0];
            rep++;
            len--;
        }
        if (m.has_kbd && id == m.kbd_id) {
            if (len >= HID_BOOT_KBD_REPORT_LEN) {
                Multiplexer::handleKeyboardReport(s->dev_idx, rep[0], &rep[2], 6);
            }
        } else if (m.has_mouse && id == m.mouse_id) {
            handle_mouse(s, rep, len, m);
        }
        return;
    }

    // Boot protocol (or descriptor not known yet): fixed layouts without report IDs.
    if (len == HID_BOOT_KBD_REPORT_LEN) {
        Multiplexer::handleKeyboardReport(s->dev_idx, rep[0], &rep[2], 6);
    } else if (len >= 3 && len <= 4) {
        ReportMap boot;
        memset(&boot, 0, sizeof(boot));
        boot.buttons = FieldLoc{true, false, 8, 0};
        boot.x = FieldLoc{true, true, 8, 8};
        boot.y = FieldLoc{true, true, 8, 16};
        if (len == 4) boot.wheel = FieldLoc{true, true, 8, 24};
        handle_mouse(s, rep, len, boot);
    }
}

static void apply_link_qos(ClassicSlot *s) {
    uint8_t status = gap_qos_set(s->con_handle, HCI_SERVICE_TYPE_BEST_EFFORT, 0xFFFFFFFF, 0xFFFFFFFF,
                                 LINK_QOS_LATENCY_US, 0xFFFFFFFF);
    if (status != ERROR_CODE_SUCCESS) {
        printf("[Classic Host] Slot %u: QoS request failed (0x%02X)\n", s->dev_idx - MAX_BLE_DEVICES, status);
    }
}

static void send_leds_to(ClassicSlot *s, uint8_t leds) {
    if (!s->in_use || !s->map.has_kbd) return;
    uint8_t id = s->map.has_led ? s->map.led_id : 0;
    // Can fail with "busy" if a previous control message is still in flight; the next lock key
    // press resends the full state.
    hid_host_send_report(s->hid_cid, id, &leds, 1);
}

// ---------------------------------------------------------------------------------------------
// Discovery and connection
// ---------------------------------------------------------------------------------------------

static void start_inquiry_if_needed() {
    if (!s_pairing || s_inquiry_active || s_connecting) return;
    if (!has_free_slot()) return;
    int status = gap_inquiry_start(INQUIRY_DURATION_1280MS);
    if (status == 0) {
        s_inquiry_active = true;
    } else {
        printf("[Classic Host] Inquiry start failed: 0x%02X\n", status);
    }
}

static void connect_to(const bd_addr_t addr, const char *name) {
    bd_addr_copy(s_connecting_addr, addr);
    snprintf(s_connecting_name, sizeof(s_connecting_name), "%s", name ? name : "");
    s_connecting = true;
    uint16_t cid = 0;
    uint8_t status = hid_host_connect((uint8_t *)addr, HID_PROTOCOL_MODE_REPORT_WITH_FALLBACK_TO_BOOT, &cid);
    if (status != ERROR_CODE_SUCCESS) {
        printf("[Classic Host] hid_host_connect(%s) failed: 0x%02X\n", bd_addr_to_str(addr), status);
        s_connecting = false;
        start_inquiry_if_needed();
    }
}

static void handle_hci_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    if (packet_type != HCI_EVENT_PACKET) return;
    bd_addr_t addr;

    switch (hci_event_packet_get_type(packet)) {
        case GAP_EVENT_INQUIRY_RESULT: {
            if (!s_pairing || s_connecting) break;
            uint32_t cod = gap_event_inquiry_result_get_class_of_device(packet);
            gap_event_inquiry_result_get_bd_addr(packet, addr);
            if ((cod & COD_MAJOR_MASK) != COD_MAJOR_PERIPHERAL) {
                printf("[Classic Host] Inquiry: ignoring %s (CoD 0x%06lX, not a peripheral)\n",
                       bd_addr_to_str(addr), (unsigned long)cod);
                break;
            }
            if (find_slot_by_addr(addr) || !has_free_slot()) break;

            char name[32] = {0};
            if (gap_event_inquiry_result_get_name_available(packet)) {
                uint8_t n = gap_event_inquiry_result_get_name_len(packet);
                if (n > sizeof(name) - 1) n = sizeof(name) - 1;
                memcpy(name, gap_event_inquiry_result_get_name(packet), n);
            }
            printf("[Classic Host] Found '%s' (%s, CoD 0x%06lX). Connecting...\n",
                   name[0] ? name : "?", bd_addr_to_str(addr), (unsigned long)cod);
            gap_inquiry_stop();
            s_inquiry_active = false;
            connect_to(addr, name);
            break;
        }

        case GAP_EVENT_INQUIRY_COMPLETE:
            printf("[Classic Host] Inquiry burst complete\n");
            s_inquiry_active = false;
            start_inquiry_if_needed();
            break;

        case HCI_EVENT_PIN_CODE_REQUEST:
            hci_event_pin_code_request_get_bd_addr(packet, addr);
            if (s_pairing || is_bonded(addr)) {
                printf("[Classic Host] PIN requested by %s, answering %s\n", bd_addr_to_str(addr), LEGACY_PIN);
                gap_pin_code_response(addr, LEGACY_PIN);
            } else {
                gap_pin_code_negative(addr);
            }
            break;

        case HCI_EVENT_USER_CONFIRMATION_REQUEST:
            hci_event_user_confirmation_request_get_bd_addr(packet, addr);
            // Pairing outside pairing mode is refused so that nearby strangers cannot bond.
            if (s_pairing || is_bonded(addr)) {
                gap_ssp_confirmation_response(addr);
            } else {
                gap_ssp_confirmation_negative(addr);
            }
            break;

        case HCI_EVENT_USER_PASSKEY_NOTIFICATION:
            // The peer is a keyboard: the user has to type this number on it, followed by Enter.
            s_passkey = hci_event_user_passkey_notification_get_numeric_value(packet);
            printf("[Classic Host] Type %06lu + Enter on the keyboard\n", (unsigned long)s_passkey);
            break;

        case HCI_EVENT_USER_PASSKEY_REQUEST:
            // The peer displays a passkey that we would have to enter, which we have no input for.
            hci_event_user_passkey_request_get_bd_addr(packet, addr);
            printf("[Classic Host] %s wants us to enter a passkey; unsupported.\n", bd_addr_to_str(addr));
            hci_send_cmd(&hci_user_passkey_request_negative_reply, addr);
            break;

        case HCI_EVENT_SIMPLE_PAIRING_COMPLETE:
            s_passkey = 0;
            printf("[Classic Host] Simple pairing complete (status 0x%02X)\n",
                   hci_event_simple_pairing_complete_get_status(packet));
            break;

        case HCI_EVENT_AUTHENTICATION_COMPLETE_EVENT:
            printf("[Classic Host] Authentication complete (status 0x%02X)\n",
                   hci_event_authentication_complete_get_status(packet));
            break;

        case HCI_EVENT_ROLE_CHANGE:
            hci_event_role_change_get_bd_addr(packet, addr);
            if (hci_event_role_change_get_status(packet) == ERROR_CODE_SUCCESS) {
                ClassicSlot *s = find_slot_by_addr(addr);
                if (s) apply_link_qos(s);
            }
            printf("[Classic Host] Role change: status 0x%02X, we are now %s\n",
                   hci_event_role_change_get_status(packet),
                   hci_event_role_change_get_role(packet) == HCI_ROLE_MASTER ? "master" : "slave");
            break;

        case HCI_EVENT_QOS_SETUP_COMPLETE:
            printf("[Classic Host] QoS setup complete: status 0x%02X\n", packet[2]);
            break;

        case HCI_EVENT_MODE_CHANGE:
            printf("[Classic Host] Mode change: status 0x%02X, mode %u (0=active 2=sniff), interval %u slots\n",
                   hci_event_mode_change_get_status(packet), hci_event_mode_change_get_mode(packet),
                   hci_event_mode_change_get_interval(packet));
            break;

        case HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE: {
            if (hci_event_remote_name_request_complete_get_status(packet) != ERROR_CODE_SUCCESS) break;
            hci_event_remote_name_request_complete_get_bd_addr(packet, addr);
            ClassicSlot *s = find_slot_by_addr(addr);
            if (s) {
                snprintf(s->name, sizeof(s->name), "%s", hci_event_remote_name_request_complete_get_remote_name(packet));
                printf("[Classic Host] Slot %u is '%s'\n", s->dev_idx - MAX_BLE_DEVICES, s->name);
            }
            break;
        }

        default:
            break;
    }
}

static void handle_hid_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    if (packet_type != HCI_EVENT_PACKET || hci_event_packet_get_type(packet) != HCI_EVENT_HID_META) return;
    bd_addr_t addr;

    switch (hci_event_hid_meta_get_subevent_code(packet)) {
        case HID_SUBEVENT_INCOMING_CONNECTION: {
            uint16_t cid = hid_subevent_incoming_connection_get_hid_cid(packet);
            hid_subevent_incoming_connection_get_address(packet, addr);
            if ((s_pairing || is_bonded(addr)) && has_free_slot()) {
                printf("[Classic Host] Incoming connection from %s accepted\n", bd_addr_to_str(addr));
                hid_host_accept_connection(cid, HID_PROTOCOL_MODE_REPORT_WITH_FALLBACK_TO_BOOT);
            } else {
                printf("[Classic Host] Incoming connection from %s declined\n", bd_addr_to_str(addr));
                hid_host_decline_connection(cid);
            }
            break;
        }

        case HID_SUBEVENT_CONNECTION_OPENED: {
            uint8_t status = hid_subevent_connection_opened_get_status(packet);
            hid_subevent_connection_opened_get_bd_addr(packet, addr);
            s_connecting = false;
            if (status != ERROR_CODE_SUCCESS) {
                printf("[Classic Host] Connection to %s failed (status 0x%02X)\n", bd_addr_to_str(addr), status);
                start_inquiry_if_needed();
                break;
            }
            ClassicSlot *s = find_slot_by_addr(addr);
            if (!s) s = alloc_slot();
            if (!s) {
                hid_host_disconnect(hid_subevent_connection_opened_get_hid_cid(packet));
                break;
            }
            s->hid_cid = hid_subevent_connection_opened_get_hid_cid(packet);
            s->con_handle = hid_subevent_connection_opened_get_con_handle(packet);
            bd_addr_copy(s->addr, addr);
            // As master we schedule the polling of the peripheral; a peripheral that paged us
            // would otherwise stay master and decide how often we can hear from it.
            printf("[Classic Host] Link role: we are %s\n",
                   gap_get_role(s->con_handle) == HCI_ROLE_MASTER ? "master" : "slave");
            if (gap_get_role(s->con_handle) != HCI_ROLE_MASTER) {
                gap_request_role(addr, HCI_ROLE_MASTER);
            }
            if (bd_addr_cmp(addr, s_connecting_addr) == 0 && s_connecting_name[0]) {
                snprintf(s->name, sizeof(s->name), "%s", s_connecting_name);
            }
            s->last_used_ms = to_ms_since_boot(get_absolute_time());
            printf("[Classic Host] Connected to %s as slot %u\n", bd_addr_to_str(addr), s->dev_idx - MAX_BLE_DEVICES);
            if (s->name[0] == '\0') {
                gap_remote_name_request(addr, 0, 0);
            }
            if (s_pairing) {
                BleHidHost::stopPairingMode();
            }
            break;
        }

        case HID_SUBEVENT_DESCRIPTOR_AVAILABLE: {
            uint16_t cid = hid_subevent_descriptor_available_get_hid_cid(packet);
            ClassicSlot *s = find_slot_by_cid(cid);
            if (!s) break;
            s->descriptor_pending = false;
            apply_link_qos(s);
            if (hid_subevent_descriptor_available_get_status(packet) == ERROR_CODE_SUCCESS) {
                parse_report_map(hid_descriptor_storage_get_descriptor_data(cid),
                                 hid_descriptor_storage_get_descriptor_len(cid), &s->map);
                printf("[Classic Host] Slot %u descriptor: kbd=%d (id %u) mouse=%d (id %u) led=%d (id %u) ids=%d\n",
                       s->dev_idx - MAX_BLE_DEVICES, s->map.has_kbd, s->map.kbd_id, s->map.has_mouse,
                       s->map.mouse_id, s->map.has_led, s->map.led_id, s->map.uses_report_ids);
            } else {
                printf("[Classic Host] Slot %u: no usable descriptor, assuming boot layouts\n",
                       s->dev_idx - MAX_BLE_DEVICES);
            }
            send_leds_to(s, Multiplexer::getHostLeds());
            break;
        }

        case HID_SUBEVENT_SET_PROTOCOL_RESPONSE: {
            ClassicSlot *s = find_slot_by_cid(hid_subevent_set_protocol_response_get_hid_cid(packet));
            if (s && hid_subevent_set_protocol_response_get_handshake_status(packet) == 0) {
                s->report_protocol =
                    hid_subevent_set_protocol_response_get_protocol_mode(packet) == HID_PROTOCOL_MODE_REPORT;
            }
            break;
        }

        case HID_SUBEVENT_REPORT: {
            ClassicSlot *s = find_slot_by_cid(hid_subevent_report_get_hid_cid(packet));
            if (s) {
                handle_report(s, hid_subevent_report_get_report(packet), hid_subevent_report_get_report_len(packet));
            }
            break;
        }

        case HID_SUBEVENT_CONNECTION_CLOSED: {
            ClassicSlot *s = find_slot_by_cid(hid_subevent_connection_closed_get_hid_cid(packet));
            if (s) {
                printf("[Classic Host] Slot %u ('%s') disconnected\n", s->dev_idx - MAX_BLE_DEVICES, s->name);
                free_slot(s);
            }
            start_inquiry_if_needed();
            break;
        }

        default:
            break;
    }
}

// ---------------------------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------------------------

void ClassicHidHost::init() {
    memset(s_slots, 0, sizeof(s_slots));

    gap_set_local_name("Pico HID Multiplexer");
    gap_set_class_of_device(0x000104);   // Computer, desktop workstation
    // Role switch lets a reconnecting peripheral's link end up with us as master, which keeps
    // concurrent BLE links scheduled sensibly.
    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_ROLE_SWITCH);
    gap_set_bondable_mode(1);
    // Display Yes/No: a keyboard shows us nothing but expects the user to type the passkey we
    // display, while display-less peripherals (keypads) fall back to "just works".
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_DISPLAY_YES_NO);
    gap_ssp_set_authentication_requirement(SSP_IO_AUTHREQ_MITM_PROTECTION_NOT_REQUIRED_GENERAL_BONDING);
    // Always connectable so bonded peripherals can reconnect when their user presses a key.
    gap_connectable_control(1);
    gap_discoverable_control(0);

    hid_host_init(s_hid_descriptor_storage, sizeof(s_hid_descriptor_storage));
    hid_host_register_packet_handler(&handle_hid_event);

    s_hci_registration.callback = &handle_hci_event;
    hci_add_event_handler(&s_hci_registration);
}

void ClassicHidHost::startPairingMode() {
    printf("[Classic Host] Pairing mode: inquiring for classic HID peripherals\n");
    s_pairing = true;
    start_inquiry_if_needed();
}

void ClassicHidHost::stopPairingMode() {
    s_pairing = false;
    s_passkey = 0;
    if (s_inquiry_active) {
        gap_inquiry_stop();
        s_inquiry_active = false;
    }
}

uint8_t ClassicHidHost::getConnectedCount() {
    uint8_t n = 0;
    for (uint8_t i = 0; i < MAX_CLASSIC_DEVICES; i++) {
        if (s_slots[i].in_use) n++;
    }
    return n;
}

bool ClassicHidHost::getSlotAddress(uint8_t dev_idx, uint8_t addr[6]) {
    for (uint8_t i = 0; i < MAX_CLASSIC_DEVICES; i++) {
        if (s_slots[i].in_use && s_slots[i].dev_idx == dev_idx) {
            memcpy(addr, s_slots[i].addr, 6);
            return true;
        }
    }
    return false;
}

const char* ClassicHidHost::getConnectedDeviceName() {
    for (uint8_t i = 0; i < MAX_CLASSIC_DEVICES; i++) {
        if (s_slots[i].in_use) return s_slots[i].name[0] ? s_slots[i].name : "BT Classic HID";
    }
    return "";
}

uint32_t ClassicHidHost::getActivePasskey() {
    return s_passkey;
}

void ClassicHidHost::sendHostLeds(uint8_t leds) {
    for (uint8_t i = 0; i < MAX_CLASSIC_DEVICES; i++) {
        send_leds_to(&s_slots[i], leds);
    }
}

void ClassicHidHost::dumpDevices() {
    printf("[Classic Host] Connected Devices (%u / %u):\n", getConnectedCount(), MAX_CLASSIC_DEVICES);
    for (uint8_t i = 0; i < MAX_CLASSIC_DEVICES; i++) {
        const ClassicSlot &s = s_slots[i];
        if (!s.in_use) continue;
        printf("  Classic slot %u: '%s' (%s, cid 0x%04X, %s protocol, kbd=%d mouse=%d led=%d, idle %lu s)\n",
               i, s.name, bd_addr_to_str(s.addr), s.hid_cid, s.report_protocol ? "report" : "boot",
               s.map.has_kbd, s.map.has_mouse, s.map.has_led,
               (unsigned long)((to_ms_since_boot(get_absolute_time()) - s.last_used_ms) / 1000));
        if (s.stat_count > 1) {
            uint32_t span_us = s.stat_last_us - s.stat_start_us;
            printf("    mouse reports since last dump: %lu in %lu ms (%lu/s), max gap %lu ms, gaps > %d ms: %lu\n",
                   (unsigned long)s.stat_count, (unsigned long)(span_us / 1000),
                   (unsigned long)((uint64_t)(s.stat_count - 1) * 1000000 / (span_us ? span_us : 1)),
                   (unsigned long)(s.stat_max_gap_us / 1000), SLOW_GAP_US / 1000,
                   (unsigned long)s.stat_slow_gaps);
            printf("    gap histogram: <2ms %lu, 2-8ms %lu, 8-16ms %lu, 16-30ms %lu, 30-60ms %lu, >60ms %lu\n",
                   (unsigned long)s.stat_hist[0], (unsigned long)s.stat_hist[1], (unsigned long)s.stat_hist[2],
                   (unsigned long)s.stat_hist[3], (unsigned long)s.stat_hist[4], (unsigned long)s.stat_hist[5]);
        }
        printf("    link: we are %s\n", gap_get_role(s.con_handle) == HCI_ROLE_MASTER ? "master" : "slave");
        s_slots[i].stat_count = 0;
        s_slots[i].stat_max_gap_us = 0;
        s_slots[i].stat_slow_gaps = 0;
        memset(s_slots[i].stat_hist, 0, sizeof(s_slots[i].stat_hist));
    }
}

void ClassicHidHost::dumpBonds() {
    btstack_link_key_iterator_t it;
    printf("[Classic Host] Bonded classic devices:\n");
    if (!gap_link_key_iterator_init(&it)) return;
    bd_addr_t a;
    link_key_t key;
    link_key_type_t type;
    for (int idx = 0; gap_link_key_iterator_get_next(&it, a, key, &type); idx++) {
        printf("  [c%d] %s%s\n", idx, bd_addr_to_str(a), find_slot_by_addr(a) ? " (connected)" : "");
    }
    gap_link_key_iterator_done(&it);
}

void ClassicHidHost::connectBonded(uint8_t bond_idx) {
    btstack_link_key_iterator_t it;
    if (!gap_link_key_iterator_init(&it)) return;
    bd_addr_t a;
    link_key_t key;
    link_key_type_t type;
    bool found = false;
    for (int idx = 0; gap_link_key_iterator_get_next(&it, a, key, &type); idx++) {
        if (idx == bond_idx) {
            found = true;
            break;
        }
    }
    gap_link_key_iterator_done(&it);
    if (!found) {
        printf("[Classic Host] No classic bond %u\n", bond_idx);
    } else if (find_slot_by_addr(a)) {
        printf("[Classic Host] Already connected\n");
    } else {
        printf("[Classic Host] Connecting to bonded %s...\n", bd_addr_to_str(a));
        connect_to(a, "");
    }
}

void ClassicHidHost::disconnectSlot(uint8_t slot_idx) {
    if (slot_idx < MAX_CLASSIC_DEVICES && s_slots[slot_idx].in_use) {
        hid_host_disconnect(s_slots[slot_idx].hid_cid);
    }
}

void ClassicHidHost::setQos(uint8_t slot_idx, uint8_t service_type, uint32_t latency_us) {
    if (slot_idx >= MAX_CLASSIC_DEVICES || !s_slots[slot_idx].in_use) {
        printf("[Classic Host] No classic slot %u\n", slot_idx);
        return;
    }
    uint8_t status = gap_qos_set(s_slots[slot_idx].con_handle, (hci_service_type_t)service_type,
                                 0xFFFFFFFF, 0xFFFFFFFF, latency_us, 0xFFFFFFFF);
    printf("[Classic Host] QoS request (type %u, latency %lu us) status 0x%02X\n", service_type,
           (unsigned long)latency_us, status);
}

void ClassicHidHost::clearBonds() {
    for (uint8_t i = 0; i < MAX_CLASSIC_DEVICES; i++) {
        if (s_slots[i].in_use) hid_host_disconnect(s_slots[i].hid_cid);
    }
    gap_delete_all_link_keys();
}
