// SPDX-License-Identifier: MIT
#include "device_store.h"
#include "nvs.h"
#include <string.h>

// Registry v2: version, count LE16, next_id LE32; id, name(33), pairing(8).
// Buffers live in BSS, not the radio task stack. Storage has one worker owner.
enum { REG_PAIR = 4 + RADIATOR_NAME_BYTES + 1 };
static uint8_t registry_data[REGISTRY_STORAGE_BYTES], registry_previous[REGISTRY_STORAGE_BYTES];
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put32(uint8_t *p, uint32_t n) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(n >> (8 * i));
}
static bool valid_record(const radiator_record *r) {
    if (!r->id || !r->name[0] || !memchr(r->name, 0, sizeof(r->name)) ||
        (r->pair.controller[0] != 0xf1 && r->pair.controller[0] != 0xd0) ||
        r->pair.controller[0] != r->pair.radiator[0] ||
        !memcmp(r->pair.controller, r->pair.radiator, 4)) return false;
    for (const char *c = r->name; *c; ++c) if ((unsigned char)*c < 32 || *c == 127) return false;
    return true;
}
static bool valid_registry(const radiator_registry *r) {
    if (!r->next_id || r->count > CLEVERBOX_RADIATOR_CAPACITY) return false;
    for (size_t i = 0; i < r->count; ++i) {
        if (!valid_record(&r->records[i]) || r->records[i].id >= r->next_id) return false;
        for (size_t j = 0; j < i; ++j)
            if (r->records[i].id == r->records[j].id ||
                !memcmp(r->records[i].pair.radiator, r->records[j].pair.radiator, 4)) return false;
    }
    return true;
}
esp_err_t device_registry_save_record(const radiator_registry *r, const radiator_record *replacement, uint32_t remove_id) {
    if (!valid_registry(r) || (replacement && (!valid_record(replacement) || remove_id))) return ESP_ERR_INVALID_ARG;
    size_t count = r->count; uint32_t next = r->next_id; bool found = false;
    for (size_t i = 0; i < r->count; ++i) {
        if ((replacement && r->records[i].id == replacement->id) || r->records[i].id == remove_id) found = true;
        if (replacement && replacement->id != r->records[i].id &&
            !memcmp(replacement->pair.radiator, r->records[i].pair.radiator, 4)) return ESP_ERR_INVALID_ARG;
    }
    if (remove_id) { if (!found) return ESP_ERR_NOT_FOUND; --count; }
    if (replacement && !found) {
        if (count == CLEVERBOX_RADIATOR_CAPACITY || replacement->id < next || replacement->id == UINT32_MAX)
            return ESP_ERR_INVALID_STATE;
        ++count; next = replacement->id + 1;
    }
    size_t size = REGISTRY_HEADER_BYTES + count * REGISTRY_RECORD_BYTES;
    memset(registry_data, 0, size);
    registry_data[0] = 2; registry_data[1] = count; registry_data[2] = count >> 8;
    put32(registry_data + 3, next);
    size_t output = 0;
    for (size_t i = 0; i < r->count + (replacement && !found ? 1U : 0U); ++i) {
        const radiator_record *d = i == r->count ? replacement : &r->records[i];
        if (d->id == remove_id) continue;
        if (replacement && d->id == replacement->id) d = replacement;
        uint8_t *b = registry_data + REGISTRY_HEADER_BYTES + output++ * REGISTRY_RECORD_BYTES;
        put32(b, d->id); memcpy(b + 4, d->name, sizeof(d->name));
        memcpy(b + REG_PAIR, &d->pair, 8);
    }
    esp_err_t result = device_mode_prepare_write();
    if (result != ESP_OK) return result;
    nvs_handle_t h; result = nvs_open("cleverbox", NVS_READWRITE, &h);
    if (result != ESP_OK) return result;
    size_t previous_size = sizeof(registry_previous);
    result = nvs_get_blob(h, "registry", registry_previous, &previous_size);
    bool absent = result == ESP_ERR_NVS_NOT_FOUND;
    if (absent) result = ESP_OK;
    // An unreadable/oversized existing record must never be overwritten.
    if (result == ESP_OK) {
        result = nvs_set_blob(h, "registry", registry_data, size);
        if (result == ESP_OK) {
            result = nvs_commit(h);
            if (result != ESP_OK) {
                esp_err_t restored = absent ? nvs_erase_key(h, "registry") :
                    nvs_set_blob(h, "registry", registry_previous, previous_size);
                if (restored == ESP_OK) restored = nvs_commit(h);
                if (restored != ESP_OK) result = ESP_ERR_INVALID_STATE;
            }
        }
    }
    nvs_close(h); return result;
}
esp_err_t device_registry_save(const radiator_registry *r) { return device_registry_save_record(r, NULL, 0); }
esp_err_t device_registry_load(radiator_registry *r) {
    memset(r, 0, sizeof(*r)); r->next_id = 1;
    nvs_handle_t h; esp_err_t result = nvs_open("cleverbox", NVS_READONLY, &h);
    if (result == ESP_OK) {
        size_t size = 0;
        result = nvs_get_blob(h, "registry", NULL, &size);
        if (result == ESP_OK && size > sizeof(registry_data)) result = DEVICE_STORE_ERR_CAPACITY;
        if (result == ESP_OK) result = nvs_get_blob(h, "registry", registry_data, &size);
        nvs_close(h);
        if (result == ESP_OK) {
            if (size < REGISTRY_HEADER_BYTES || registry_data[0] != 2) return ESP_ERR_INVALID_STATE;
            size_t stride = REGISTRY_RECORD_BYTES;
            size_t count = registry_data[1] | (size_t)registry_data[2] << 8;
            if (count > CLEVERBOX_RADIATOR_CAPACITY) return DEVICE_STORE_ERR_CAPACITY;
            if (size != REGISTRY_HEADER_BYTES + count * stride) return ESP_ERR_INVALID_STATE;
            r->count = count; r->next_id = get32(registry_data + 3);
            for (size_t i = 0; i < count; ++i) {
                radiator_record *d = &r->records[i]; const uint8_t *b = registry_data + REGISTRY_HEADER_BYTES + i * stride;
                d->id = get32(b); memcpy(d->name, b + 4, sizeof(d->name));
                memcpy(&d->pair, b + REG_PAIR, 8);
            }
            if (!valid_registry(r)) { r->count = 0; return ESP_ERR_INVALID_STATE; }
            return ESP_OK;
        }
    }
    if (result != ESP_ERR_NVS_NOT_FOUND && result != ESP_ERR_NOT_FOUND) return result;
    return ESP_OK;
}
