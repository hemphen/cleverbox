// SPDX-License-Identifier: MIT
#include "network_setup.h"
#include "nvs.h"
#include "mode_store.h"
#include <string.h>
// Byte arrays only: versioned and independent of compiler padding.
typedef struct { unsigned char version; network_credentials credentials; } wifi_record;
static bool persistence_uncertain; // Cleared by a verified forced write, or process restart.
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
// Preserve raw bytes too: explicit provisioning/forget may repair a corrupt key.
// Oversized foreign/corrupt blobs cannot be backed up within this bounded store.
typedef struct { wifi_record record; size_t size; bool present, restorable; } snapshot;
static esp_err_t read_snapshot(snapshot *s) {
    memset(s, 0, sizeof(*s)); s->restorable = true;
    nvs_handle_t h; esp_err_t err = nvs_open("cleverbox", NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    err = nvs_get_blob(h, "wifi", NULL, &s->size);
    if (err == ESP_ERR_NVS_NOT_FOUND) { s->size = 0; err = ESP_OK; }
    else if (err == ESP_OK) {
        s->present = true;
        if (s->size > sizeof(s->record)) s->restorable = false;
        else err = nvs_get_blob(h, "wifi", &s->record, &s->size);
    }
    nvs_close(h); return err;
}
static bool matches(const snapshot *expected) {
    snapshot actual;
    bool same = read_snapshot(&actual) == ESP_OK && actual.restorable &&
        actual.present == expected->present && actual.size == expected->size &&
        !memcmp(&actual.record, &expected->record, actual.size);
    memset(&actual, 0, sizeof(actual)); return same;
}
static bool restore(const snapshot *previous) {
    if (!previous->restorable) return false;
    nvs_handle_t h;
    if (nvs_open("cleverbox", NVS_READWRITE, &h) != ESP_OK) return false;
    // Do not accept an unchanged cached old value as proof of restoration: NVS
    // can defer completion of a failed write until reinitialization. Erase before
    // rewriting so the same-value optimization cannot skip the restoring write.
    esp_err_t err = nvs_erase_key(h, "wifi");
    bool erased = err == ESP_OK;
    if (erased) err = nvs_commit(h);
    else if (err == ESP_ERR_NVS_NOT_FOUND && previous->present) err = ESP_OK;
    if (err == ESP_OK && previous->present) {
        err = nvs_set_blob(h, "wifi", &previous->record, previous->size);
        if (err == ESP_OK) err = nvs_commit(h);
    }
    nvs_close(h);
    // For a previously absent key, a cached NOT_FOUND alone cannot rule out a
    // failed new value becoming visible at restart. Report uncertainty instead.
    return err == ESP_OK && (previous->present || erased) && matches(previous);
}
static esp_err_t replace(const snapshot *desired) {
    esp_err_t mode_error = device_mode_prepare_write();
    if (mode_error != ESP_OK) return mode_error;
    snapshot previous;
    esp_err_t err = read_snapshot(&previous);
    if (err != ESP_OK) {
        memset(&previous, 0, sizeof(previous));
        return persistence_uncertain ? NETWORK_ERR_PERSISTENCE_UNCERTAIN : err;
    }
    // After an unresolved error, an ordinary same-value write or absent-key
    // forget could be a cached no-op. Require a fresh restoring write instead.
    if (persistence_uncertain) {
        bool restored = restore(desired);
        memset(&previous, 0, sizeof(previous));
        if (restored) { persistence_uncertain = false; return ESP_OK; }
        return NETWORK_ERR_PERSISTENCE_UNCERTAIN;
    }
    nvs_handle_t h;
    err = nvs_open("cleverbox", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = desired->present ? nvs_set_blob(h, "wifi", &desired->record, desired->size)
                               : nvs_erase_key(h, "wifi");
        // Forget of an already absent key is an idempotent success.
        if (!desired->present && err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
        else if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
        if (err == ESP_OK && !matches(desired)) err = ESP_FAIL;
        if (err != ESP_OK && !restore(&previous)) {
            persistence_uncertain = true; err = NETWORK_ERR_PERSISTENCE_UNCERTAIN;
        }
    }
    memset(&previous, 0, sizeof(previous)); return err;
}
esp_err_t network_store_save(const network_credentials *c) {
    if (!network_credentials_valid(c)) return ESP_ERR_INVALID_ARG;
    snapshot desired = {.record = {.version = 1, .credentials = *c},
                        .size = sizeof(wifi_record), .present = true, .restorable = true};
    esp_err_t err = replace(&desired); memset(&desired, 0, sizeof(desired)); return err;
}
esp_err_t network_store_forget(void) {
    snapshot desired = {.restorable = true}; return replace(&desired);
}
