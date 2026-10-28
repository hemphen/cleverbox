// SPDX-License-Identifier: MIT
#include "device_store.h"
#include "nvs.h"
#include <string.h>

esp_err_t device_store_load(paired_device *device)
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

esp_err_t device_store_save(const paired_device *device)
{
    uint8_t data[9] = {1}; // Versioned bytes, independent of C structure padding.
    memcpy(data + 1, device->controller, 4);
    memcpy(data + 5, device->radiator, 4);
    nvs_handle_t handle;
    esp_err_t result = nvs_open("cleverbox", NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;
    result = nvs_set_blob(handle, "pairing", data, sizeof(data));
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result;
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

esp_err_t device_store_load_state(const paired_device *device, rf_frame *state)
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

esp_err_t device_store_save_state(const paired_device *device, const rf_frame *state)
{
    if (state && !valid_state(device, state)) return ESP_ERR_INVALID_ARG;
    uint8_t data[STATE_RECORD_SIZE] = {1}, previous[STATE_RECORD_SIZE] = {1};
    memcpy(data + 1, device->controller, 4);
    memcpy(data + 5, device->radiator, 4);
    memcpy(previous, data, 9);
    if (state) memcpy(data + 9, state->bytes, 59);
    nvs_handle_t handle;
    esp_err_t result = nvs_open("cleverbox", NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;
    size_t previous_size = sizeof(previous);
    result = read_state(handle, previous, &previous_size);
    if (result == ESP_ERR_NOT_FOUND) { previous_size = 9; result = ESP_OK; }
    // Do not overwrite an unreadable record (e.g. a future oversized version).
    if (result == ESP_OK) {
        result = nvs_set_blob(handle, "control_state", data, state ? sizeof(data) : 9);
        if (result == ESP_OK) {
            result = nvs_commit(handle);
            if (result != ESP_OK) {
                // NVS may write during set_blob; close alone is not rollback.
                // Restore the prior bytes on a commit failure, including absence.
                esp_err_t restored = nvs_set_blob(handle, "control_state", previous, previous_size);
                if (restored == ESP_OK) restored = nvs_commit(handle);
                if (restored != ESP_OK) result = ESP_ERR_INVALID_STATE;
            }
        }
    }
    nvs_close(handle);
    return result;
}
