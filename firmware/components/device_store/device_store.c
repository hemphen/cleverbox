// SPDX-License-Identifier: MIT
#include "device_store.h"
#include "nvs.h"
#include <string.h>

static esp_err_t load_legacy_pair(paired_device *device)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open("cleverbox", NVS_READONLY, &handle);
    if (result != ESP_OK) return result == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : result;
    uint8_t data[9];
    size_t size = sizeof(data);
    result = nvs_get_blob(handle, "pairing", data, &size);
    nvs_close(handle);
    if (result != ESP_OK) return result == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : result;
    if (size != sizeof(data) || data[0] != 1 ||
        (data[1] != 0xf1 && data[1] != 0xd0) || data[1] != data[5] ||
        !memcmp(data + 1, data + 5, 4)) return ESP_ERR_INVALID_STATE;
    memcpy(device->controller, data + 1, 4);
    memcpy(device->radiator, data + 5, 4);
    return ESP_OK;
}

// v1: version + pairing identity + the exact validated 59-byte frame.
// A nine-byte identity-only tombstone clears state without touching pairing.
#define STATE_RECORD_SIZE 68
static bool valid_state(const paired_device *device, const rf_frame *state)
{
    rf_frame check;
    return rf_set_target(&check, state, RF_OFF, 0) &&
           !memcmp(state->bytes + 6, device->radiator, 4);
}

static esp_err_t read_state(nvs_handle_t handle, uint8_t data[STATE_RECORD_SIZE], size_t *size)
{
    esp_err_t result = nvs_get_blob(handle, "control_state", data, size);
    return result == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : result;
}

static esp_err_t load_legacy_state(const paired_device *device, rf_frame *state)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open("cleverbox", NVS_READONLY, &handle);
    if (result != ESP_OK) return result == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : result;
    uint8_t data[STATE_RECORD_SIZE];
    size_t size = sizeof(data);
    result = read_state(handle, data, &size);
    nvs_close(handle);
    if (result != ESP_OK) return result;
    if ((size != 9 && size != sizeof(data)) || data[0] != 1 ||
        memcmp(data + 1, device->controller, 4) || memcmp(data + 5, device->radiator, 4))
        return ESP_ERR_INVALID_STATE;
    if (size == 9) return ESP_ERR_NOT_FOUND;
    rf_frame loaded = {.size = 59};
    memcpy(loaded.bytes, data + 9, 59);
    if (!valid_state(device, &loaded)) return ESP_ERR_INVALID_STATE;
    *state = loaded;
    return ESP_OK;
}

// Registry v1: version(1), count LE16, next_id LE32, then fixed wire records:
// id LE32, NUL-terminated name(33), pairing(8), saved flag(1), state(59).
// Buffers live in BSS, not the radio task stack. Storage has one worker owner.
enum { REG_PAIR = 4 + RADIATOR_NAME_BYTES + 1, REG_SAVED = REG_PAIR + 8, REG_STATE = REG_SAVED + 1 };
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
    if (r->has_saved_template) {
        rf_frame f = {.size = CONTROLLER_STATE_BYTES};
        memcpy(f.bytes, r->saved_template, CONTROLLER_STATE_BYTES);
        if (!valid_state(&r->pair, &f)) return false;
    }
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
    registry_data[0] = 1; registry_data[1] = count; registry_data[2] = count >> 8;
    put32(registry_data + 3, next);
    size_t output = 0;
    for (size_t i = 0; i < r->count + (replacement && !found ? 1U : 0U); ++i) {
        const radiator_record *d = i == r->count ? replacement : &r->records[i];
        if (d->id == remove_id) continue;
        if (replacement && d->id == replacement->id) d = replacement;
        uint8_t *b = registry_data + REGISTRY_HEADER_BYTES + output++ * REGISTRY_RECORD_BYTES;
        put32(b, d->id); memcpy(b + 4, d->name, sizeof(d->name));
        memcpy(b + REG_PAIR, &d->pair, 8); b[REG_SAVED] = d->has_saved_template;
        if (d->has_saved_template) memcpy(b + REG_STATE, d->saved_template, CONTROLLER_STATE_BYTES);
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
            if (size < REGISTRY_HEADER_BYTES || registry_data[0] != 1) return ESP_ERR_INVALID_STATE;
            size_t count = registry_data[1] | (size_t)registry_data[2] << 8;
            if (count > CLEVERBOX_RADIATOR_CAPACITY) return DEVICE_STORE_ERR_CAPACITY;
            if (size != REGISTRY_HEADER_BYTES + count * REGISTRY_RECORD_BYTES) return ESP_ERR_INVALID_STATE;
            r->count = count; r->next_id = get32(registry_data + 3);
            for (size_t i = 0; i < count; ++i) {
                radiator_record *d = &r->records[i]; const uint8_t *b = registry_data + REGISTRY_HEADER_BYTES + i * REGISTRY_RECORD_BYTES;
                d->id = get32(b); memcpy(d->name, b + 4, sizeof(d->name));
                memcpy(&d->pair, b + REG_PAIR, 8);
                if (b[REG_SAVED] > 1) { r->count = 0; return ESP_ERR_INVALID_STATE; }
                d->has_saved_template = b[REG_SAVED]; memcpy(d->saved_template, b + REG_STATE, CONTROLLER_STATE_BYTES);
            }
            if (!valid_registry(r)) { r->count = 0; return ESP_ERR_INVALID_STATE; }
            return ESP_OK;
        }
    }
    if (result != ESP_ERR_NVS_NOT_FOUND && result != ESP_ERR_NOT_FOUND) return result;
    // Repeatable migration: legacy keys stay intact until AND after a committed
    // registry exists. A committed empty registry prevents removed-device resurrection.
    paired_device pair; result = load_legacy_pair(&pair);
    if (result == ESP_ERR_NOT_FOUND) return ESP_OK;
    if (result != ESP_OK) return result;
    radiator_record *d = &r->records[0]; d->id = 1; d->pair = pair;
    memcpy(d->name, "Radiator 1", 11); r->count = 1; r->next_id = 2;
    rf_frame state = {0}; result = load_legacy_state(&pair, &state);
    if (result == ESP_OK) {
        d->has_saved_template = true; memcpy(d->saved_template, state.bytes, CONTROLLER_STATE_BYTES);
    } else if (result != ESP_ERR_NOT_FOUND) {
        // Retain usable pairing and expose the invalid optional legacy state.
        // Explicit import/clear can repair it; do not migrate it away silently.
        d->state_load_error = result; return result;
    }
    return device_registry_save(r);
}
