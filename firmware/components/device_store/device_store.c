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
