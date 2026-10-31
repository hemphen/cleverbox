// SPDX-License-Identifier: MIT
#include "mode_store.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdint.h>

// Two-byte v1 mode record. A failed write may have reached flash: callers must
// fail closed and report uncertainty instead of promising rollback.
static esp_err_t save_record(bool enabled, bool retry_initial) {
    nvs_handle_t h;
    esp_err_t e = nvs_open("cleverbox", NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    uint8_t data[2] = {1, enabled ? 1 : 0}, check[2] = {0};
    // Only a failed initial bootstrap is retried this way. Settings have not
    // been allowed through the gate yet, so temporary absence cannot turn a
    // fresh installation into a legacy one. Force a physical rewrite rather
    // than trusting a same-value NVS cache left by the failed first attempt.
    if (retry_initial) {
        e = nvs_erase_key(h, "rf_mode");
        if (e == ESP_OK) e = nvs_commit(h);
        else if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
    }
    if (e == ESP_OK) e = nvs_set_blob(h, "rf_mode", data, sizeof(data));
    if (e == ESP_OK) e = nvs_commit(h);
    size_t n = sizeof(check);
    if (e == ESP_OK) e = nvs_get_blob(h, "rf_mode", check, &n);
    if (e == ESP_OK && (n != sizeof(check) || memcmp(data, check, n))) e = ESP_ERR_INVALID_STATE;
    nvs_close(h);
    return e;
}
esp_err_t device_mode_load(bool *enabled, bool *stored) {
    *enabled = false; *stored = false;
    nvs_handle_t h;
    esp_err_t e = nvs_open("cleverbox", NVS_READONLY, &h);
    if (e == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (e != ESP_OK) return e;
    uint8_t data[2]; size_t n = sizeof(data);
    e = nvs_get_blob(h, "rf_mode", data, &n);
    if (e == ESP_OK) {
        if (n != sizeof(data) || data[0] != 1 || data[1] > 1) e = ESP_ERR_INVALID_STATE;
        else { *enabled = data[1] != 0; *stored = true; }
    } else if (e == ESP_ERR_NVS_NOT_FOUND) {
        // Presence, even an empty registry or invalid older key, identifies an
        // installation. Read errors are not evidence of a fresh installation.
        static const char *keys[] = {"registry", "pairing", "control_state", "wifi", "timezone", "mqtt", "ha_discovery"};
        bool existing = false;
        for (size_t i = 0; i < sizeof(keys)/sizeof(keys[0]); ++i) {
            n = 0; e = nvs_get_blob(h, keys[i], NULL, &n);
            if (e == ESP_OK) existing = true;
            else if (e != ESP_ERR_NVS_NOT_FOUND) break;
        }
        if (e == ESP_ERR_NVS_NOT_FOUND || e == ESP_OK) { e = ESP_OK; *enabled = existing; }
    }
    nvs_close(h);
    return e;
}

// Start once before any settings owner. Retain the decision even when its first
// write fails: settings created in this boot must never become migration evidence.
static SemaphoreHandle_t mode_mutex;
static bool started, initial_enabled, decision_valid, initial_pending;
static esp_err_t initial_result = ESP_ERR_INVALID_STATE;

esp_err_t device_mode_start(bool *enabled) {
    // Only startup calls the first initialization, before tasks/settings owners.
    if (!started) {
        started = true;
        mode_mutex = xSemaphoreCreateMutex();
        if (!mode_mutex) { initial_result = ESP_ERR_NO_MEM; *enabled = false; return initial_result; }
        bool stored = false;
        initial_result = device_mode_load(&initial_enabled, &stored);
        decision_valid = initial_result == ESP_OK;
        initial_pending = decision_valid && !stored;
        if (initial_pending) {
            initial_result = save_record(initial_enabled, false);
            if (initial_result == ESP_OK) initial_pending = false;
        }
    }
    // Repeated startup consumers see the same boot decision/result, never a
    // second migration scan after another task has configured the device.
    *enabled = initial_result == ESP_OK && initial_enabled;
    return initial_result;
}

static bool mode_record_present(void) {
    // A known existing (even invalid) mode record cannot become absent-key
    // migration evidence. Allow Wi-Fi/settings repair without overwriting it;
    // RF remains closed by the latched load error. Unknown presence blocks writes.
    nvs_handle_t h;
    if (nvs_open("cleverbox", NVS_READONLY, &h) != ESP_OK) return false;
    size_t n = 0;
    esp_err_t e = nvs_get_blob(h, "rf_mode", NULL, &n);
    nvs_close(h);
    return e == ESP_OK;
}

esp_err_t device_mode_prepare_write(void) {
    if (!mode_mutex) return initial_result;
    xSemaphoreTake(mode_mutex, portMAX_DELAY);
    esp_err_t e = decision_valid || mode_record_present() ? ESP_OK : initial_result;
    if (decision_valid && initial_pending) {
        e = save_record(initial_enabled, true);
        if (e == ESP_OK) initial_pending = false;
    }
    xSemaphoreGive(mode_mutex);
    return e;
}

esp_err_t device_mode_save(bool enabled) {
    if (!mode_mutex) return initial_result;
    xSemaphoreTake(mode_mutex, portMAX_DELAY);
    esp_err_t e = save_record(enabled, initial_pending);
    if (e == ESP_OK) {
        decision_valid = true;
        initial_pending = false;
    }
    xSemaphoreGive(mode_mutex);
    return e;
}
