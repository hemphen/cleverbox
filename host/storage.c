// SPDX-License-Identifier: MIT
// File-backed NVS: each key is stored in "<CLEVERBOX_SIM_STORAGE>.<key>".
#include "nvs.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int path(const char *key, char *out, size_t size)
{
    const char *base = getenv("CLEVERBOX_SIM_STORAGE");
    return base && snprintf(out, size, "%s.%s", base, key) < (int)size;
}

esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *h)
{
    (void)mode; *h = 1;
    return !strcmp(name, "cleverbox") && getenv("CLEVERBOX_SIM_STORAGE") ? ESP_OK : ESP_FAIL;
}

esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *size)
{
    (void)h;
    char name[4096];
    if (!path(key, name, sizeof(name))) return ESP_FAIL;
    FILE *f = fopen(name, "rb");
    if (!f) return errno == ENOENT ? ESP_ERR_NVS_NOT_FOUND : ESP_FAIL;
    fseek(f, 0, SEEK_END);
    size_t length = (size_t)ftell(f);
    rewind(f);
    esp_err_t e = ESP_OK;
    if (out && length > *size) e = ESP_ERR_INVALID_SIZE;
    else if (out && fread(out, 1, length, f) != length) e = ESP_FAIL;
    fclose(f);
    if (e == ESP_OK) *size = length;
    return e;
}

esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *data, size_t size)
{
    (void)h;
    char name[4096];
    if (!path(key, name, sizeof(name))) return ESP_FAIL;
    FILE *f = fopen(name, "wb");
    if (!f) return ESP_FAIL;
    size_t written = fwrite(data, 1, size, f);
    return fclose(f) == 0 && written == size ? ESP_OK : ESP_FAIL;
}

esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    (void)h;
    char name[4096];
    if (!path(key, name, sizeof(name))) return ESP_FAIL;
    if (remove(name) == 0) return ESP_OK;
    return errno == ENOENT ? ESP_ERR_NVS_NOT_FOUND : ESP_FAIL;
}

esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }
