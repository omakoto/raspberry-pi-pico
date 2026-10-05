#include "vial_server.h"
#include "virtual_matrix.h"
#include "vial_layout.h"
#include "config.h"
#include "pico/time.h"
#include <stdio.h>
#include <string.h>
#include "log_ring.h"

// VIA Commands
#define VIA_CMD_GET_PROTOCOL_VERSION        0x01
#define VIA_CMD_GET_KEYBOARD_VALUE          0x02
#define VIA_CMD_SET_KEYBOARD_VALUE          0x03
#define VIA_CMD_DYNAMIC_KEYMAP_GET_KEYCODE  0x04
#define VIA_CMD_DYNAMIC_KEYMAP_SET_KEYCODE  0x05
#define VIA_CMD_DYNAMIC_KEYMAP_RESET        0x06
#define VIA_CMD_BOOTLOADER_JUMP             0x0B
#define VIA_CMD_MACRO_GET_COUNT             0x0C
#define VIA_CMD_MACRO_GET_BUFFER_SIZE       0x0D
#define VIA_CMD_DYNAMIC_KEYMAP_GET_LAYER_COUNT 0x11
#define VIA_CMD_KEYMAP_GET_BUFFER           0x12
#define VIA_CMD_KEYMAP_SET_BUFFER           0x13

// VIAL Sub-commands (Prefix 0xFE)
#define VIAL_PREFIX                         0xFE

// Own commands to read the previous run's log without a serial port (see lastlog.py).
//   0xFD 0x00:                 info reply: [0xFD, 0x00, valid, watchdog, len(4, LE), uptime_ms(4, LE),
//                              stage(2, LE), crumb count, crumbs(2 each, LE, oldest first)]
//   0xFD 0x01 <offset(2, LE)>: log chunk reply: [0xFD, 0x01, count, 0, offset(2, LE), data...]
#define DEBUG_PREFIX                        0xFD
#define VIAL_CMD_GET_KEYBOARD_ID            0x00
#define VIAL_CMD_GET_SIZE                   0x01
#define VIAL_CMD_GET_DEF_CHUNK              0x02
#define VIAL_CMD_GET_ENCODER                0x03
#define VIAL_CMD_GET_UNLOCK_STATUS          0x05
#define VIAL_CMD_UNLOCK_START               0x06
#define VIAL_CMD_UNLOCK_POLL                0x07
#define VIAL_CMD_LOCK                       0x08
#define VIAL_CMD_QMK_SETTINGS_QUERY         0x09
#define VIAL_CMD_QMK_SETTINGS_GET           0x0A
#define VIAL_CMD_QMK_SETTINGS_SET           0x0B
#define VIAL_CMD_QMK_SETTINGS_RESET         0x0C
#define VIAL_CMD_DYNAMIC_ENTRY_OP           0x0D

bool VialServer::bootloader_requested_ = false;

bool VialServer::bootloaderRequested() {
    return bootloader_requested_;
}

void VialServer::init() {
    VirtualMatrix::init();
}

void VialServer::handleRawReport(const uint8_t *in_buf, uint8_t *out_buf) {
    if (!in_buf || !out_buf) return;

    if (in_buf[0] == DEBUG_PREFIX) {
        handleDebugCommand(in_buf, out_buf);
    } else if (in_buf[0] == VIAL_PREFIX) {
        handleVialCommand(in_buf, out_buf);
    } else {
        handleViaCommand(in_buf, out_buf);
    }
}

void VialServer::handleViaCommand(const uint8_t *in_buf, uint8_t *out_buf) {
    uint8_t cmd = in_buf[0];
    memcpy(out_buf, in_buf, 32);

    switch (cmd) {
        case VIA_CMD_GET_PROTOCOL_VERSION: // 0x01
            out_buf[0] = 0x01;
            out_buf[1] = 0x00;
            out_buf[2] = 0x09; // Protocol Version 9
            break;

        case VIA_CMD_GET_KEYBOARD_VALUE: { // 0x02
            uint8_t val_id = in_buf[1];
            if (val_id == 0x01) { // Uptime
                uint32_t ms = to_ms_since_boot(get_absolute_time());
                out_buf[2] = (ms >> 24) & 0xFF;
                out_buf[3] = (ms >> 16) & 0xFF;
                out_buf[4] = (ms >> 8) & 0xFF;
                out_buf[5] = ms & 0xFF;
            } else if (val_id == 0x02) { // Layout options
                out_buf[2] = 0;
                out_buf[3] = 0;
                out_buf[4] = 0;
                out_buf[5] = 0;
            } else {
                out_buf[0] = 0xFF; // Unhandled
            }
            break;
        }

        case VIA_CMD_SET_KEYBOARD_VALUE: // 0x03
            // Echo
            break;

        case VIA_CMD_DYNAMIC_KEYMAP_GET_KEYCODE: { // 0x04
            uint8_t layer = in_buf[1];
            uint8_t row = in_buf[2];
            uint8_t col = in_buf[3];
            uint16_t kc = VirtualMatrix::getKeycode(layer, row, col);
            out_buf[4] = (kc >> 8) & 0xFF;
            out_buf[5] = kc & 0xFF;
            break;
        }

        case VIA_CMD_DYNAMIC_KEYMAP_SET_KEYCODE: { // 0x05
            uint8_t layer = in_buf[1];
            uint8_t row = in_buf[2];
            uint8_t col = in_buf[3];
            uint16_t kc = ((uint16_t)in_buf[4] << 8) | in_buf[5];
            VirtualMatrix::setKeycode(layer, row, col, kc);
            break;
        }

        case VIA_CMD_DYNAMIC_KEYMAP_RESET: // 0x06
            VirtualMatrix::resetKeymap();
            break;

        case VIA_CMD_BOOTLOADER_JUMP: // 0x0B
            // The reply (an echo) goes out first; the main loop then reboots.
            bootloader_requested_ = true;
            break;

        case VIA_CMD_MACRO_GET_COUNT: // 0x0C
            out_buf[0] = 0x0C;
            out_buf[1] = 0; // 0 macros
            break;

        case VIA_CMD_MACRO_GET_BUFFER_SIZE: // 0x0D
            out_buf[0] = 0x0D;
            out_buf[1] = 0; // Size MSB = 0
            out_buf[2] = 0; // Size LSB = 0
            break;

        case VIA_CMD_DYNAMIC_KEYMAP_GET_LAYER_COUNT: // 0x11
            out_buf[0] = 0x11;
            out_buf[1] = NUM_LAYERS;
            break;

        case VIA_CMD_KEYMAP_GET_BUFFER: { // 0x12
            uint16_t offset = ((uint16_t)in_buf[1] << 8) | in_buf[2];
            uint8_t sz = in_buf[3];
            if (sz > 28) sz = 28;
            out_buf[0] = 0x12;
            out_buf[1] = in_buf[1];
            out_buf[2] = in_buf[2];
            out_buf[3] = sz;
            for (uint8_t i = 0; i < sz; i++) {
                uint16_t byte_idx = offset + i;
                uint16_t key_idx = byte_idx / 2;
                uint8_t layer = key_idx / (MATRIX_ROWS * MATRIX_COLS);
                uint8_t rem = key_idx % (MATRIX_ROWS * MATRIX_COLS);
                uint8_t row = rem / MATRIX_COLS;
                uint8_t col = rem % MATRIX_COLS;
                uint16_t kc = (layer < NUM_LAYERS) ? VirtualMatrix::getKeycode(layer, row, col) : 0;
                out_buf[4 + i] = (byte_idx % 2 == 0) ? ((kc >> 8) & 0xFF) : (kc & 0xFF);
            }
            break;
        }

        case VIA_CMD_KEYMAP_SET_BUFFER: { // 0x13
            uint16_t offset = ((uint16_t)in_buf[1] << 8) | in_buf[2];
            uint8_t sz = in_buf[3];
            if (sz > 28) sz = 28;
            for (uint8_t i = 0; i + 1 < sz; i += 2) {
                uint16_t byte_idx = offset + i;
                uint16_t key_idx = byte_idx / 2;
                uint8_t layer = key_idx / (MATRIX_ROWS * MATRIX_COLS);
                uint8_t rem = key_idx % (MATRIX_ROWS * MATRIX_COLS);
                uint8_t row = rem / MATRIX_COLS;
                uint8_t col = rem % MATRIX_COLS;
                uint16_t kc = ((uint16_t)in_buf[4 + i] << 8) | in_buf[4 + i + 1];
                if (layer < NUM_LAYERS) {
                    VirtualMatrix::setKeycode(layer, row, col, kc);
                }
            }
            break;
        }

        default:
            printf("[Vial] Unhandled VIA command 0x%02X\n", cmd);
            out_buf[0] = 0xFF; // Unhandled
            break;
    }
}

void VialServer::handleDebugCommand(const uint8_t *in_buf, uint8_t *out_buf) {
    memset(out_buf, 0, 32);
    out_buf[0] = DEBUG_PREFIX;
    out_buf[1] = in_buf[1];
    uint32_t len = 0;
    const uint8_t *log = LogRing::previousLog(&len);

    if (in_buf[1] == 0x00) {
        out_buf[2] = LogRing::previousRunValid();
        out_buf[3] = LogRing::previousRunWasWatchdog();
        for (int i = 0; i < 4; i++) out_buf[4 + i] = (uint8_t)(len >> (8 * i));
        uint32_t up = LogRing::previousUptimeMs();
        for (int i = 0; i < 4; i++) out_buf[8 + i] = (uint8_t)(up >> (8 * i));
        uint16_t stage = LogRing::previousStage();
        out_buf[12] = (uint8_t)stage;
        out_buf[13] = (uint8_t)(stage >> 8);
        uint16_t crumbs[LOG_CRUMB_COUNT];
        uint8_t n = LogRing::previousBreadcrumbs(crumbs);
        if (n > 8) n = 8;  // only the last 8 fit in one packet
        uint16_t all[LOG_CRUMB_COUNT];
        uint8_t total = LogRing::previousBreadcrumbs(all);
        out_buf[14] = n;
        for (uint8_t i = 0; i < n; i++) {
            uint16_t c = all[total - n + i];
            out_buf[15 + 2 * i] = (uint8_t)c;
            out_buf[16 + 2 * i] = (uint8_t)(c >> 8);
        }
    } else if (in_buf[1] == 0x01) {
        uint32_t offset = in_buf[2] | ((uint32_t)in_buf[3] << 8);
        uint32_t count = (offset < len) ? len - offset : 0;
        if (count > 24) count = 24;
        out_buf[2] = (uint8_t)count;
        out_buf[4] = in_buf[2];
        out_buf[5] = in_buf[3];
        if (count > 0) memcpy(&out_buf[8], log + offset, count);
    }
}

void VialServer::handleVialCommand(const uint8_t *in_buf, uint8_t *out_buf) {
    uint8_t sub_cmd = in_buf[1];
    memset(out_buf, 0, 32);

    switch (sub_cmd) {
        case VIAL_CMD_GET_KEYBOARD_ID: // 0x00
            // 4-byte protocol version (v3 = 0x00000003, little endian)
            out_buf[0] = 0x03;
            out_buf[1] = 0x00;
            out_buf[2] = 0x00;
            out_buf[3] = 0x00;
            // 8-byte UID: 'P','I','C','O','2','W','M','X'
            out_buf[4] = 'P';
            out_buf[5] = 'I';
            out_buf[6] = 'C';
            out_buf[7] = 'O';
            out_buf[8] = '2';
            out_buf[9] = 'W';
            out_buf[10] = 'M';
            out_buf[11] = 'X';
            break;

        case VIAL_CMD_GET_SIZE: { // 0x01
            uint32_t def_size = VIAL_KEYBOARD_DEF_SIZE;
            out_buf[0] = def_size & 0xFF;
            out_buf[1] = (def_size >> 8) & 0xFF;
            out_buf[2] = (def_size >> 16) & 0xFF;
            out_buf[3] = (def_size >> 24) & 0xFF;
            break;
        }

        case VIAL_CMD_GET_DEF_CHUNK: { // 0x02
            uint32_t page = in_buf[2] | ((uint32_t)in_buf[3] << 8);
            uint32_t offset = page * 32;
            if (offset < VIAL_KEYBOARD_DEF_SIZE) {
                uint32_t rem = VIAL_KEYBOARD_DEF_SIZE - offset;
                uint32_t chunk_len = (rem > 32) ? 32 : rem;
                memcpy(out_buf, VIAL_KEYBOARD_DEF + offset, chunk_len);
            }
            break;
        }

        case VIAL_CMD_GET_ENCODER: // 0x03
            // No rotary encoders
            break;

        case VIAL_CMD_GET_UNLOCK_STATUS: // 0x05
            out_buf[0] = 1; // 1 = Unlocked
            out_buf[1] = 0; // Unlock not in progress
            break;

        case VIAL_CMD_UNLOCK_START: // 0x06
        case VIAL_CMD_UNLOCK_POLL:  // 0x07
            out_buf[0] = 1; // Already unlocked
            break;

        case VIAL_CMD_LOCK: // 0x08
            // Always unlocked; nothing to lock.
            break;

        case VIAL_CMD_QMK_SETTINGS_QUERY: // 0x09
            // The client pages through the supported QSIDs until it sees 0xFFFF. We support none, so
            // answer with an immediate terminator; any other reply makes it poll forever.
            memset(out_buf, 0xFF, 32);
            break;

        case VIAL_CMD_QMK_SETTINGS_GET:   // 0x0A
        case VIAL_CMD_QMK_SETTINGS_SET:   // 0x0B
        case VIAL_CMD_QMK_SETTINGS_RESET: // 0x0C
            // No settings; the all-zero reply is a valid "nothing" answer.
            break;

        case VIAL_CMD_DYNAMIC_ENTRY_OP: // 0x0D
            // Sub-op 0 asks for the tap dance / combo / key override entry counts (bytes 0..2). We
            // have none, so the all-zero reply is right; a non-zero count would make the client
            // request that many entries.
            break;

        default:
            printf("[Vial] Unhandled VIAL sub-command 0x%02X\n", sub_cmd);
            out_buf[0] = 0xFF; // Unhandled
            break;
    }
}
