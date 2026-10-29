// SPDX-License-Identifier: MIT
// File-backed NVS subset: device_store.c still owns record format/validation.
#include "nvs.h"
#include "device_store.h"
#include "mqtt_bridge.h"
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
static unsigned char staged[REGISTRY_STORAGE_BYTES > MQTT_RECORD_BYTES ? REGISTRY_STORAGE_BYTES : MQTT_RECORD_BYTES];
static size_t staged_size;
static char selected_path[4096];
static int staged_valid;
// Test-only flash boundary faults; production network store owns rollback.
// 1: mutation becomes durable then reports failure; 2: rollback also fails.
static int wifi_fault, wifi_fault_used;
int host_wifi_storage_fault(int mode) {
    if (mode < 0 || mode > 2) return 0;
    wifi_fault = mode; wifi_fault_used = 0; return 1;
}
static const char *path(void) { return getenv("CLEVERBOX_SIM_STORAGE"); }
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *h) {
    if (strcmp(name, "cleverbox") || !path()) return ESP_FAIL;
    (void)mode;
    staged_valid = 0; *h = 1; return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *n) {
    (void)h;
    if (strcmp(key, "pairing") && strcmp(key, "control_state") && strcmp(key, "registry") && strcmp(key, "wifi") && strcmp(key, "timezone") && strcmp(key, "mqtt")) return ESP_FAIL;
    char filename[4096];
    if (snprintf(filename, sizeof(filename), "%s%s", path(), !strcmp(key, "mqtt") ? ".mqtt" : !strcmp(key, "timezone") ? ".timezone" : !strcmp(key, "wifi") ? ".wifi" : !strcmp(key, "pairing") ? "" : !strcmp(key, "registry") ? ".registry" : ".state") >= (int)sizeof(filename)) return ESP_FAIL;
    FILE *f = fopen(filename, "rb");
    if (!f) return errno == ENOENT ? ESP_ERR_NVS_NOT_FOUND : ESP_FAIL;
    if (!out) {
        if (fseek(f, 0, SEEK_END)) { fclose(f); return ESP_FAIL; }
        long length = ftell(f); fclose(f);
        if (length < 0) return ESP_FAIL;
        *n = (size_t)length; return ESP_OK;
    }
    size_t read = fread(out, 1, *n, f);
    int extra = fgetc(f), failed = ferror(f); fclose(f);
    if (failed || extra != EOF) return ESP_ERR_INVALID_STATE;
    *n = read; return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *in, size_t n) {
    (void)h;
    if ((strcmp(key, "pairing") && strcmp(key, "control_state") && strcmp(key, "registry") && strcmp(key, "wifi") && strcmp(key, "timezone") && strcmp(key, "mqtt")) || n > sizeof(staged)) return ESP_FAIL;
    if (snprintf(selected_path, sizeof(selected_path), "%s%s", path(), !strcmp(key, "mqtt") ? ".mqtt" : !strcmp(key, "timezone") ? ".timezone" : !strcmp(key, "wifi") ? ".wifi" : !strcmp(key, "pairing") ? "" : !strcmp(key, "registry") ? ".registry" : ".state") >= (int)sizeof(selected_path)) return ESP_FAIL;
    if (!strcmp(key, "wifi") && wifi_fault == 2 && wifi_fault_used) return ESP_FAIL;
    memcpy(staged, in, n); staged_size = n; staged_valid = 1;
    if (!strcmp(key, "wifi") && wifi_fault && !wifi_fault_used) {
        wifi_fault_used = 1;
        esp_err_t err = nvs_commit(h); return err == ESP_OK ? ESP_FAIL : err;
    }
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) {
    (void)h;
    if (!staged_valid) return ESP_FAIL;
    if (staged_valid == 2) return remove(selected_path) == 0 || errno == ENOENT ? ESP_OK : ESP_FAIL;
    char temporary[4096];
    if (snprintf(temporary, sizeof(temporary), "%s.tmp", selected_path) >= (int)sizeof(temporary)) return ESP_FAIL;
    FILE *f = fopen(temporary, "wb"); if (!f) return ESP_FAIL;
    int failed = fwrite(staged, 1, staged_size, f) != staged_size;
    if (fclose(f)) failed = 1;
    if (failed || rename(temporary, selected_path)) { remove(temporary); return ESP_FAIL; }
    return ESP_OK;
}
void nvs_close(nvs_handle_t h) { (void)h; staged_valid = 0; }

esp_err_t nvs_erase_key(nvs_handle_t h, const char *key) {
    (void)h;
    if (strcmp(key, "registry") && strcmp(key, "wifi") && strcmp(key, "timezone") && strcmp(key, "mqtt")) return ESP_FAIL;
    if (snprintf(selected_path, sizeof(selected_path), "%s%s", path(), !strcmp(key, "mqtt") ? ".mqtt" : !strcmp(key, "timezone") ? ".timezone" : !strcmp(key, "wifi") ? ".wifi" : ".registry") >= (int)sizeof(selected_path)) return ESP_FAIL;
    if (!strcmp(key, "wifi") && wifi_fault == 2 && wifi_fault_used) return ESP_FAIL;
    staged_valid = 2;
    if (!strcmp(key, "wifi") && wifi_fault && !wifi_fault_used) {
        wifi_fault_used = 1;
        esp_err_t err = nvs_commit(h); return err == ESP_OK ? ESP_FAIL : err;
    }
    return ESP_OK;
}
