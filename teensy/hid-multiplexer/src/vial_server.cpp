#include "vial_server.h"
#include "virtual_matrix.h"
#include "vial_layout.h"
#include "multiplexer.h"
#include "usb_host_manager.h"
#include "logger.h"
#include <Arduino.h>
#include "usb_rawhid.h"
#include "core_pins.h"

// VIA Protocol Commands
#define VIA_CMD_GET_PROTOCOL_VERSION      0x01
#define VIA_CMD_GET_KEYBOARD_VALUE       0x02
#define VIA_CMD_SET_KEYBOARD_VALUE       0x03
#define VIA_CMD_DYNAMIC_KEYMAP_GET_KEYCODE 0x04
#define VIA_CMD_DYNAMIC_KEYMAP_SET_KEYCODE 0x05
#define VIA_CMD_DYNAMIC_KEYMAP_RESET     0x06
#define VIA_CMD_CUSTOM_SAVE              0x07
#define VIA_CMD_MACRO_GET_COUNT          0x0C
#define VIA_CMD_MACRO_GET_BUFFER_SIZE    0x0D
#define VIA_CMD_MACRO_GET_BUFFER         0x0E
#define VIA_CMD_MACRO_SET_BUFFER         0x0F
#define VIA_CMD_DYNAMIC_KEYMAP_GET_LAYER_COUNT 0x11
#define VIA_CMD_KEYMAP_GET_BUFFER        0x12
#define VIA_CMD_KEYMAP_SET_BUFFER        0x13

// VIAL Extended Commands (prefixed by 0xFE)
#define VIAL_PREFIX                      0xFE
#define VIAL_CMD_GET_KEYBOARD_ID         0x00
#define VIAL_CMD_GET_SIZE                0x01
#define VIAL_CMD_GET_DEF_CHUNK           0x02
#define VIAL_CMD_GET_ENCODER             0x03
#define VIAL_CMD_SET_ENCODER             0x04
#define VIAL_CMD_GET_UNLOCK_STATUS       0x05
#define VIAL_CMD_UNLOCK_START            0x06
#define VIAL_CMD_UNLOCK_POLL             0x07
#define VIAL_CMD_LOCK                    0x08

static void checkStream(Stream &stream, char *buf, uint8_t &pos, size_t max_len) {
    while (stream.available()) {
        char c = (char)stream.read();
        if (c == '\r' || c == '\n') {
            if (pos > 0) {
                buf[pos] = '\0';
                VialServer::processSerialCommand(buf, stream);
                pos = 0;
            }
        } else if (pos < max_len - 1) {
            // Only accept printable ASCII in command buffer
            if (c >= 32 && c <= 126) {
                buf[pos++] = c;
            }
        }
    }
}

void VialServer::init() {
    logger_println("[VialServer] Initialized WebHID VIAL/VIA protocol & CLI server.");
}

void VialServer::poll() {
#if defined(RAWHID_INTERFACE)
    uint8_t rx_buf[32];
    uint8_t tx_buf[32];
    // Check for incoming 32-byte Raw HID packet from WebHID (vial.rocks / usevia.app)
    int n = RawHID.recv(rx_buf, 0);
    if (n > 0) {
        memset(tx_buf, 0, sizeof(tx_buf));
        handleRawHidPacket(rx_buf, tx_buf);
        RawHID.send(tx_buf, 50);
    }
#endif

    // Check for incoming serial commands from USB CDC Serial
    static char usb_cmd_buf[64];
    static uint8_t usb_cmd_pos = 0;
    checkStream(Serial, usb_cmd_buf, usb_cmd_pos, sizeof(usb_cmd_buf));

    // Check for incoming serial commands from Hardware UART1 (Pins 0/1)
    static char uart_cmd_buf[64];
    static uint8_t uart_cmd_pos = 0;
    checkStream(Serial1, uart_cmd_buf, uart_cmd_pos, sizeof(uart_cmd_buf));
}

void VialServer::handleRawHidPacket(const uint8_t *in_buf, uint8_t *out_buf) {
    if (in_buf[0] == VIAL_PREFIX) {
        handleVialCommand(in_buf, out_buf);
    } else {
        handleViaCommand(in_buf, out_buf);
    }
}

void VialServer::handleViaCommand(const uint8_t *in_buf, uint8_t *out_buf) {
    uint8_t cmd = in_buf[0];
    memcpy(out_buf, in_buf, 32); // Default echo

    switch (cmd) {
        case VIA_CMD_GET_PROTOCOL_VERSION: // 0x01
            out_buf[0] = 0x01; // Echo command
            out_buf[1] = 0x00; // VIA v9 MSB
            out_buf[2] = 0x09; // VIA v9 LSB
            break;

        case VIA_CMD_GET_KEYBOARD_VALUE: { // 0x02
            uint8_t val_id = in_buf[1];
            if (val_id == 0x01) { // Uptime
                uint32_t ms = millis();
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
            uint16_t key = VirtualMatrix::getKeycode(layer, row, col);
            out_buf[4] = (key >> 8) & 0xFF;
            out_buf[5] = key & 0xFF;
            break;
        }

        case VIA_CMD_DYNAMIC_KEYMAP_SET_KEYCODE: { // 0x05
            uint8_t layer = in_buf[1];
            uint8_t row = in_buf[2];
            uint8_t col = in_buf[3];
            uint16_t key = ((uint16_t)in_buf[4] << 8) | in_buf[5];
            VirtualMatrix::setKeycode(layer, row, col, key);
            break;
        }

        case VIA_CMD_DYNAMIC_KEYMAP_RESET: // 0x06
            VirtualMatrix::resetKeymap();
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
            out_buf[0] = 0xFF; // Unhandled
            break;
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
            // 8-byte UID
            out_buf[4] = 0x54; // 'T'
            out_buf[5] = 0x45; // 'E'
            out_buf[6] = 0x45; // 'E'
            out_buf[7] = 0x4E; // 'N'
            out_buf[8] = 0x53; // 'S'
            out_buf[9] = 0x59; // 'Y'
            out_buf[10] = 0x04;
            out_buf[11] = 0x01;
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
                memcpy_P(out_buf, VIAL_KEYBOARD_DEF + offset, chunk_len);
            }
            break;
        }

        case VIAL_CMD_GET_ENCODER: // 0x03
            // No encoders
            break;

        case VIAL_CMD_GET_UNLOCK_STATUS: // 0x05
            out_buf[0] = 1; // 1 = Unlocked
            out_buf[1] = 0; // Unlock not in progress
            break;

        case VIAL_CMD_UNLOCK_START: // 0x06
        case VIAL_CMD_UNLOCK_POLL:  // 0x07
            out_buf[0] = 1; // Already unlocked
            break;

        default:
            out_buf[0] = 0xFF; // Unhandled
            break;
    }
}

void VialServer::processSerialCommand(const char *cmd_line) {
    processSerialCommand(cmd_line, Serial);
}

void VialServer::processSerialCommand(const char *cmd_line, Stream &stream) {
    if (cmd_line == nullptr) return;

    // Skip leading spaces
    while (*cmd_line && *cmd_line == ' ') cmd_line++;
    if (*cmd_line == '\0') return;

    // Guard against potential serial echo loops by ignoring log output prefixes
    if (strncmp(cmd_line, "Unknown command:", 16) == 0 ||
        strncmp(cmd_line, "---", 3) == 0 ||
        cmd_line[0] == '[' ||
        strstr(cmd_line, "Keyboard") != nullptr ||
        strstr(cmd_line, "Mouse") != nullptr ||
        strstr(cmd_line, "EHCI") != nullptr ||
        strstr(cmd_line, "Host") != nullptr ||
        strstr(cmd_line, "Resource") != nullptr ||
        strstr(cmd_line, "Initiating") != nullptr) {
        return;
    }

    if (strncmp(cmd_line, "bootloader", 10) == 0 || strncmp(cmd_line, "reboot", 6) == 0) {
        logger_println("[System] Rebooting into HalfKay bootloader...");
        logger_flush();
        delay(50);
        _reboot_Teensyduino_();
    } else if (strncmp(cmd_line, "help", 4) == 0) {
        logger_println("\n--- Teensy HID Multiplexer Commands ---");
        logger_println("  status               - Show layer, host LEDs, and USB Host diagnostics");
        logger_println("  devices              - Show detailed USB Host device and endpoint diagnostics");
        logger_println("  usb_reset            - Power-cycle VBUS and reset USB Host bus");
        logger_println("  bootloader / reboot  - Enter HalfKay bootloader for flashing");
        logger_println("  dump <layer>         - Dump keycodes for layer 0-3");
        logger_println("  remap <L> <R> <C> <K>- Remap (Layer, Row, Col, HexKeycode)");
        logger_println("  reset                - Reset keymap to 1:1 defaults");
        logger_println("  help                 - Display this help message");
    } else if (strncmp(cmd_line, "status", 6) == 0) {
        logger_printf("\n[Status] Uptime: %lu ms\n", millis());
        logger_printf("[Status] Active Layer: %u\n", VirtualMatrix::getActiveLayer());
        logger_printf("[Status] Host CapsLock: %s, NumLock: %s\n", 
            (Multiplexer::getHostLeds() & 2) ? "ON" : "OFF",
            (Multiplexer::getHostLeds() & 1) ? "ON" : "OFF");
        UsbHostManager::printDiagnostics();
    } else if (strncmp(cmd_line, "devices", 7) == 0) {
        UsbHostManager::printDiagnostics();
    } else if (strncmp(cmd_line, "usb_reset", 9) == 0) {
        UsbHostManager::resetBus();
    } else if (strncmp(cmd_line, "reset", 5) == 0) {
        VirtualMatrix::resetKeymap();
        logger_println("[Status] Keymap reset to 1:1 defaults.");
    } else if (strncmp(cmd_line, "dump", 4) == 0) {
        int layer = 0;
        sscanf(cmd_line + 4, "%d", &layer);
        if (layer < 0 || layer >= NUM_LAYERS) layer = 0;
        logger_printf("\n--- Keymap Dump (Layer %d) ---\n", layer);
        for (int r = 0; r < MATRIX_ROWS; r++) {
            logger_printf("R%02d: ", r);
            for (int c = 0; c < MATRIX_COLS; c++) {
                logger_printf("%04X ", VirtualMatrix::getKeycode(layer, r, c));
            }
            logger_println();
        }
    } else if (strncmp(cmd_line, "remap", 5) == 0) {
        unsigned int l, r, c, k;
        if (sscanf(cmd_line + 5, "%u %u %u %x", &l, &r, &c, &k) == 4) {
            VirtualMatrix::setKeycode(l, r, c, k);
            logger_printf("[Remap] Layer %u (%u, %u) -> 0x%04X saved.\n", l, r, c, k);
        } else {
            logger_println("Usage: remap <Layer> <Row> <Col> <HexKeycode>");
        }
    } else {
        // Send unknown command notification only to the stream that received it
        stream.printf("Unknown command: %s (type 'help' for options)\r\n", cmd_line);
    }
}
