// SPDX-License-Identifier: MIT
#include "device_time.h"
#include "nvs.h"
#include <string.h>
// Separate versioned configuration only; no timestamp, sync state or flash writes
// on SNTP notifications. String occupies only its used length on flash.
esp_err_t time_store_load(char out[DEVICE_TZ_BYTES + 1]) {
    unsigned char record[DEVICE_TZ_BYTES + 3] = {0}; size_t n = sizeof(record);
    nvs_handle_t h; esp_err_t err = nvs_open("cleverbox", NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    err = nvs_get_blob(h, "timezone", record, &n); nvs_close(h);
    if (err != ESP_OK) return err;
    if (n < 3 || n > sizeof(record) || record[0] != 1 || record[n-1] ||
        strlen((char *)record + 1) != n - 2 || !device_timezone_valid((char *)record + 1)) return ESP_ERR_INVALID_STATE;
    strcpy(out, (char *)record + 1); return ESP_OK;
}
esp_err_t time_store_save(const char *tz) {
    if (!device_timezone_valid(tz)) return ESP_ERR_INVALID_ARG;
    unsigned char record[DEVICE_TZ_BYTES + 3] = {1}; memcpy(record + 1, tz, strlen(tz) + 1);
    nvs_handle_t h; esp_err_t err = nvs_open("cleverbox", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    // A failed write may become effective only on NVS reinitialization, while
    // the cache still contains the old rule. Force erase/commit then rewrite:
    // same-value set/readback alone cannot resolve that uncertainty. Saves are
    // explicit and infrequent, so always force the write instead of tracking a
    // second uncertainty flag in the store. No rollback is promised on failure.
    err = nvs_erase_key(h, "timezone");
    if (err == ESP_OK) err = nvs_commit(h);
    else if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_set_blob(h, "timezone", record, strlen(tz) + 2);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        char verified[DEVICE_TZ_BYTES + 1]; err = time_store_load(verified);
        if (err == ESP_OK && strcmp(tz, verified)) err = ESP_FAIL;
    }
    return err;
}
