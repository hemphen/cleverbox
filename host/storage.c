// SPDX-License-Identifier: MIT
// File-backed NVS subset: device_store.c still owns record format/validation.
#include "nvs.h"
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
static unsigned char staged[68];
static size_t staged_size;
static char selected_path[4096];
static int staged_valid;
static const char *path(void) { return getenv("CLEVERBOX_SIM_STORAGE"); }
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *h) {
    if (strcmp(name, "cleverbox") || !path()) return ESP_FAIL;
    if (mode == NVS_READONLY) {
        FILE *f = fopen(path(), "rb");
        if (!f) return errno == ENOENT ? ESP_ERR_NVS_NOT_FOUND : ESP_FAIL;
        fclose(f);
    }
    staged_valid = 0; *h = 1; return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *n) {
    (void)h;
    if (strcmp(key, "pairing") && strcmp(key, "control_state")) return ESP_FAIL;
    char filename[4096];
    if (snprintf(filename, sizeof(filename), "%s%s", path(), !strcmp(key, "pairing") ? "" : ".state") >= (int)sizeof(filename)) return ESP_FAIL;
    FILE *f = fopen(filename, "rb");
    if (!f) return errno == ENOENT ? ESP_ERR_NVS_NOT_FOUND : ESP_FAIL;
    size_t read = fread(out, 1, *n, f);
    int extra = fgetc(f), failed = ferror(f); fclose(f);
    if (failed || extra != EOF) return ESP_ERR_INVALID_STATE;
    *n = read; return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *in, size_t n) {
    (void)h;
    if ((strcmp(key, "pairing") && strcmp(key, "control_state")) || n > sizeof(staged)) return ESP_FAIL;
    if (snprintf(selected_path, sizeof(selected_path), "%s%s", path(), !strcmp(key, "pairing") ? "" : ".state") >= (int)sizeof(selected_path)) return ESP_FAIL;
    memcpy(staged, in, n); staged_size = n; staged_valid = 1; return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) {
    (void)h;
    if (!staged_valid) return ESP_FAIL;
    char temporary[4096];
    if (snprintf(temporary, sizeof(temporary), "%s.tmp", selected_path) >= (int)sizeof(temporary)) return ESP_FAIL;
    FILE *f = fopen(temporary, "wb"); if (!f) return ESP_FAIL;
    int failed = fwrite(staged, 1, staged_size, f) != staged_size;
    if (fclose(f)) failed = 1;
    if (failed || rename(temporary, selected_path)) { remove(temporary); return ESP_FAIL; }
    return ESP_OK;
}
void nvs_close(nvs_handle_t h) { (void)h; staged_valid = 0; }
