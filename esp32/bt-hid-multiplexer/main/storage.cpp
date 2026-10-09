#include "storage.h"
#include "bindings_format.h"
#include "log_ring.h"

#include <stdio.h>
#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"

// NVS layout (namespace NVS_NAMESPACE):
//   "km_hdr"      KeymapHeader
//   "km0".."km7"  one blob per layer (MATRIX_ROWS * MATRIX_COLS uint16_t keycodes)
//   "bind"        BindingsBlob
//   "macros"      the VIAL macro buffer (up to MACRO_BUFFER_SIZE bytes)
// One blob per layer keeps a typical VIAL edit (one key) to a 512-byte write. NVS checksums every
// entry itself; the magic and version detect a layout change.

struct KeymapHeader {
    uint32_t magic;
    uint16_t version;
    uint8_t layers;
    uint8_t reserved;
};

static const size_t LAYER_BYTES = sizeof(uint16_t) * MATRIX_ROWS * MATRIX_COLS;

static nvs_handle_t s_nvs;
static bool s_nvs_open = false;

// What is stored in NVS, so that saveKeymap() only rewrites the layers that changed.
static uint16_t s_stored[NUM_LAYERS][MATRIX_ROWS][MATRIX_COLS];
static bool s_stored_valid = false;

static void layer_key(char out[8], int layer) {
    snprintf(out, 8, "km%d", layer);
}

void StorageManager::init() {
    if (s_nvs_open) return;
    // BTstack's TLV has normally initialized NVS already, in which case this is a no-op.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        printf("[Storage] NVS unusable (%s), erasing it\n", esp_err_to_name(err));
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        printf("[Storage] nvs_flash_init failed: %s\n", esp_err_to_name(err));
        return;
    }
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        printf("[Storage] nvs_open failed: %s\n", esp_err_to_name(err));
        return;
    }
    s_nvs_open = true;
}

bool StorageManager::loadKeymap(uint16_t keymap[NUM_LAYERS][MATRIX_ROWS][MATRIX_COLS], bool &needs_save) {
    needs_save = false;
    if (!s_nvs_open) return false;

    KeymapHeader hdr;
    size_t len = sizeof(hdr);
    if (nvs_get_blob(s_nvs, "km_hdr", &hdr, &len) != ESP_OK || len != sizeof(hdr) ||
        hdr.magic != FLASH_KEYMAP_MAGIC || hdr.version != FLASH_KEYMAP_VERSION || hdr.layers != NUM_LAYERS) {
        return false;
    }
    for (int l = 0; l < NUM_LAYERS; l++) {
        char key[8];
        layer_key(key, l);
        len = LAYER_BYTES;
        if (nvs_get_blob(s_nvs, key, keymap[l], &len) != ESP_OK || len != LAYER_BYTES) {
            return false;
        }
    }
    memcpy(s_stored, keymap, sizeof(s_stored));
    s_stored_valid = true;
    return true;
}

void StorageManager::saveKeymap(const uint16_t keymap[NUM_LAYERS][MATRIX_ROWS][MATRIX_COLS]) {
    if (!s_nvs_open) return;
    LogRing::breadcrumb(CRUMB_FLASH_KEYMAP_BEGIN);
    esp_err_t err = ESP_OK;
    for (int l = 0; l < NUM_LAYERS && err == ESP_OK; l++) {
        if (s_stored_valid && memcmp(s_stored[l], keymap[l], LAYER_BYTES) == 0) continue;
        char key[8];
        layer_key(key, l);
        err = nvs_set_blob(s_nvs, key, keymap[l], LAYER_BYTES);
    }
    if (err == ESP_OK && !s_stored_valid) {
        KeymapHeader hdr = {FLASH_KEYMAP_MAGIC, FLASH_KEYMAP_VERSION, NUM_LAYERS, 0};
        err = nvs_set_blob(s_nvs, "km_hdr", &hdr, sizeof(hdr));
    }
    if (err == ESP_OK) err = nvs_commit(s_nvs);
    LogRing::breadcrumb(CRUMB_FLASH_KEYMAP_END);

    if (err != ESP_OK) {
        printf("[Storage] Saving the keymap failed: %s\n", esp_err_to_name(err));
        s_stored_valid = false;  // unknown state: rewrite everything next time
        return;
    }
    memcpy(s_stored, keymap, sizeof(s_stored));
    s_stored_valid = true;
}

void StorageManager::clearKeymap() {
    if (!s_nvs_open) return;
    nvs_erase_key(s_nvs, "km_hdr");
    nvs_commit(s_nvs);
    s_stored_valid = false;
}

bool StorageManager::loadBindings(DeviceBindingEntry entries[MAX_DEVICE_BINDINGS], bool &needs_save) {
    needs_save = false;
    if (!s_nvs_open) return false;
    // Static: too large for the bt_app task's stack. bt_app is the only caller. NVS reports the
    // stored size, which tells the versions apart (the current blob is the largest).
    static BindingsBlob blob;
    size_t len = sizeof(blob);
    if (nvs_get_blob(s_nvs, "bind", &blob, &len) != ESP_OK) return false;
    if (!bindings_decode(&blob, len, entries, needs_save)) return false;
    if (needs_save) printf("[Storage] Converted the device bindings from an older version\n");
    return true;
}

void StorageManager::saveBindings(const DeviceBindingEntry entries[MAX_DEVICE_BINDINGS]) {
    if (!s_nvs_open) return;
    // Static: too large for the bt_app task's stack. bt_app is the only caller.
    static BindingsBlob blob;
    bindings_encode(entries, &blob);

    LogRing::breadcrumb(CRUMB_FLASH_BINDINGS_BEGIN);
    esp_err_t err = nvs_set_blob(s_nvs, "bind", &blob, sizeof(blob));
    if (err == ESP_OK) err = nvs_commit(s_nvs);
    LogRing::breadcrumb(CRUMB_FLASH_BINDINGS_END);
    if (err != ESP_OK) {
        printf("[Storage] Saving the device bindings failed: %s\n", esp_err_to_name(err));
    }
}

bool StorageManager::loadMacros(uint8_t *buffer) {
    if (!s_nvs_open) return false;
    size_t len = MACRO_BUFFER_SIZE;
    if (nvs_get_blob(s_nvs, "macros", buffer, &len) != ESP_OK) return false;
    // A buffer saved by a build with a smaller MACRO_BUFFER_SIZE holds the same macros: the rest
    // of the buffer is empty macros.
    memset(buffer + len, 0, MACRO_BUFFER_SIZE - len);
    return true;
}

void StorageManager::saveMacros(const uint8_t *buffer) {
    if (!s_nvs_open) return;
    esp_err_t err = nvs_set_blob(s_nvs, "macros", buffer, MACRO_BUFFER_SIZE);
    if (err == ESP_OK) err = nvs_commit(s_nvs);
    if (err != ESP_OK) {
        printf("[Storage] Saving the macros failed: %s\n", esp_err_to_name(err));
    }
}
