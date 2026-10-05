#include "vial_definition.h"
#include "vial_layout.h"
#include "config.h"

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------------------------
// .xz container (format: https://tukaani.org/xz/xz-file-format.txt)
// ---------------------------------------------------------------------------------------------

static uint32_t crc32(const uint8_t *data, size_t len, uint32_t crc = 0) {
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

namespace {

// Appends bytes to a buffer and remembers whether it ran out of room.
struct Writer {
    uint8_t *out;
    size_t size;
    size_t pos = 0;
    bool overflow = false;

    void put(uint8_t b) {
        if (pos < size) out[pos] = b; else overflow = true;
        pos++;
    }
    void put(const uint8_t *data, size_t len) {
        for (size_t i = 0; i < len; i++) put(data[i]);
    }
    void put32le(uint32_t v) {
        for (int i = 0; i < 4; i++) put((uint8_t)(v >> (8 * i)));
    }
    void putVarint(uint64_t v) {
        while (v >= 0x80) {
            put((uint8_t)(v | 0x80));
            v >>= 7;
        }
        put((uint8_t)v);
    }
    void padTo4(size_t start) {
        while ((pos - start) % 4 != 0) put(0);
    }
    uint32_t crcFrom(size_t start) const {
        return overflow ? 0 : crc32(out + start, pos - start);
    }
};

}  // namespace

size_t xz_wrap_uncompressed(const uint8_t *data, size_t len, uint8_t *out, size_t out_size) {
    Writer w{out, out_size};

    // Stream header: magic, stream flags (check type 1 = CRC32), CRC32 of the flags.
    static const uint8_t magic[6] = {0xFD, '7', 'z', 'X', 'Z', 0x00};
    static const uint8_t stream_flags[2] = {0x00, 0x01};
    w.put(magic, sizeof(magic));
    w.put(stream_flags, sizeof(stream_flags));
    w.put32le(crc32(stream_flags, sizeof(stream_flags)));

    // Block header: size, flags (one filter, no optional sizes), filter LZMA2 (0x21) with one
    // property byte (dictionary size; 0 = 4 KiB, irrelevant for uncompressed chunks), padding, CRC32.
    size_t header_start = w.pos;
    w.put(0);  // header size, patched below
    w.put(0x00);
    w.put(0x21);
    w.put(0x01);
    w.put(0x00);
    w.padTo4(header_start);
    size_t header_size = w.pos - header_start + 4;
    if (!w.overflow) out[header_start] = (uint8_t)(header_size / 4 - 1);
    w.put32le(w.crcFrom(header_start));

    // LZMA2 data: uncompressed chunks of at most 64 KiB (control 0x01 resets the dictionary for the
    // first one, 0x02 for the rest), then the end marker.
    size_t data_start = w.pos;
    for (size_t off = 0; off < len; ) {
        size_t n = len - off;
        if (n > 0x10000) n = 0x10000;
        w.put(off == 0 ? 0x01 : 0x02);
        w.put((uint8_t)((n - 1) >> 8));
        w.put((uint8_t)(n - 1));
        w.put(data + off, n);
        off += n;
    }
    w.put(0x00);
    size_t compressed_size = w.pos - data_start;
    w.padTo4(data_start);
    w.put32le(crc32(data, len));
    size_t unpadded_size = header_size + compressed_size + 4;

    // Index: indicator, one record (unpadded size, uncompressed size), padding, CRC32.
    size_t index_start = w.pos;
    w.put(0x00);
    w.putVarint(1);
    w.putVarint(unpadded_size);
    w.putVarint(len);
    w.padTo4(index_start);
    w.put32le(w.crcFrom(index_start));
    size_t index_size = w.pos - index_start;

    // Stream footer: CRC32 of (backward size, stream flags), backward size, stream flags, magic.
    uint8_t footer[6];
    uint32_t backward = (uint32_t)(index_size / 4 - 1);
    for (int i = 0; i < 4; i++) footer[i] = (uint8_t)(backward >> (8 * i));
    footer[4] = stream_flags[0];
    footer[5] = stream_flags[1];
    w.put32le(crc32(footer, sizeof(footer)));
    w.put(footer, sizeof(footer));
    w.put('Y');
    w.put('Z');

    return w.overflow ? 0 : w.pos;
}

// ---------------------------------------------------------------------------------------------
// Definition JSON
// ---------------------------------------------------------------------------------------------

// Appends a JSON string literal. Only printable ASCII is kept: a Bluetooth name cut off in the middle
// of a UTF-8 sequence would make the whole definition invalid, and VIAL would refuse the keyboard.
static void put_json_string(Writer &w, const char *s) {
    w.put('"');
    for (; *s; s++) {
        uint8_t c = (uint8_t)*s;
        if (c == '"' || c == '\\') {
            w.put('\\');
            w.put(c);
        } else if (c >= 0x20 && c < 0x7F) {
            w.put(c);
        } else {
            w.put('?');
        }
    }
    w.put('"');
}

static void put_text(Writer &w, const char *s) {
    w.put((const uint8_t *)s, strlen(s));
}

size_t vial_build_definition(const VialDeviceEntry *devices, uint8_t count, uint8_t *out, size_t out_size) {
    // Static: too large for the bt_app task's stack. bt_app is the only caller.
    static uint8_t json[4096];
    Writer w{json, sizeof(json)};
    put_text(w, VIAL_DEF_PREFIX);
    put_text(w, "\"labels\":[");
    if (count > VIAL_MAX_DEVICE_OPTIONS) count = VIAL_MAX_DEVICE_OPTIONS;
    for (uint8_t i = 0; i < count; i++) {
        if (i > 0) w.put(',');
        w.put('[');
        char label[48];
        const uint8_t *a = devices[i].addr;
        if (devices[i].name[0] == '\0') {
            snprintf(label, sizeof(label), "%02X:%02X:%02X:%02X:%02X:%02X", a[0], a[1], a[2], a[3], a[4], a[5]);
        } else {
            // Two devices of the same model would be indistinguishable: add the end of the address.
            bool duplicate = false;
            for (uint8_t j = 0; j < count; j++) {
                if (j != i && strcmp(devices[j].name, devices[i].name) == 0) duplicate = true;
            }
            if (duplicate) {
                snprintf(label, sizeof(label), "%.31s (%02X:%02X)", devices[i].name, a[4], a[5]);
            } else {
                snprintf(label, sizeof(label), "%s", devices[i].name);
            }
        }
        put_json_string(w, label);
        put_text(w, ",\"No binding\"");
        for (int layer = 1; layer < (1 << VIAL_OPTION_BITS); layer++) {
            char choice[16];
            snprintf(choice, sizeof(choice), ",\"Layer %d\"", layer);
            put_text(w, choice);
        }
        w.put(']');
    }
    put_text(w, "],");
    put_text(w, VIAL_DEF_SUFFIX);
    if (w.overflow) return 0;
    return xz_wrap_uncompressed(json, w.pos, out, out_size);
}

// ---------------------------------------------------------------------------------------------
// Layout options
// ---------------------------------------------------------------------------------------------

uint32_t vial_pack_layout_options(const uint8_t *choices, uint8_t count) {
    uint32_t value = 0;
    for (uint8_t i = 0; i < count; i++) {
        value = (value << VIAL_OPTION_BITS) | (choices[i] & ((1u << VIAL_OPTION_BITS) - 1));
    }
    return value;
}

void vial_unpack_layout_options(uint32_t value, uint8_t count, uint8_t *choices) {
    for (uint8_t i = 0; i < count; i++) {
        uint8_t shift = (uint8_t)(VIAL_OPTION_BITS * (count - 1 - i));
        choices[i] = (uint8_t)((value >> shift) & ((1u << VIAL_OPTION_BITS) - 1));
    }
}
