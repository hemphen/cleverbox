// SPDX-License-Identifier: MIT
#include "network_setup.h"
#include "nvs.h"
#include <string.h>
// Byte arrays only: versioned and independent of compiler padding.
typedef struct { unsigned char version; network_credentials credentials; } wifi_record;
esp_err_t network_store_load(network_credentials *out) {
    memset(out, 0, sizeof(*out));
    nvs_handle_t h; esp_err_t err = nvs_open("cleverbox", NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    wifi_record record = {0}; size_t n = sizeof(record);
    err = nvs_get_blob(h, "wifi", &record, &n); nvs_close(h);
    if (err == ESP_OK && (n != sizeof(record) || record.version != 1 ||
                         !network_credentials_valid(&record.credentials))) err = ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) *out = record.credentials;
    memset(&record, 0, sizeof(record)); return err;
}
esp_err_t network_store_save(const network_credentials *c) {
    if (!network_credentials_valid(c)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h; esp_err_t err = nvs_open("cleverbox", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    wifi_record record = {.version = 1, .credentials = *c};
    err = nvs_set_blob(h, "wifi", &record, sizeof(record));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h); memset(&record, 0, sizeof(record)); return err;
}
esp_err_t network_store_forget(void) {
    nvs_handle_t h; esp_err_t err = nvs_open("cleverbox", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_erase_key(h, "wifi");
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    else if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h); return err;
}
