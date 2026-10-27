// SPDX-License-Identifier: MIT
#include "device_store.h"
#include "nvs.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// Fake NVS boundary: exercise the actual store code, including commit failures.
static unsigned char disk[9], pending[9];
static bool exists, fail_write, fail_commit;
static int closes;
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *out)
{
    assert(!strcmp(name, "cleverbox")); *out = 1;
    return mode == NVS_READONLY && !exists ? ESP_ERR_NVS_NOT_FOUND : ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *size)
{
    assert(h == 1 && !strcmp(key, "pairing") && *size == sizeof(disk));
    memcpy(out, disk, sizeof(disk)); return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *in, size_t size)
{
    assert(h == 1 && !strcmp(key, "pairing") && size == sizeof(pending));
    if (fail_write) return ESP_FAIL;
    memcpy(pending, in, size); return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h)
{
    assert(h == 1);
    if (fail_commit) return ESP_FAIL;
    memcpy(disk, pending, sizeof(disk)); exists = true; return ESP_OK;
}
void nvs_close(nvs_handle_t h) { assert(h == 1); ++closes; }
int main(void)
{
    paired_device device = {{0xf1,1,2,3}, {0xf1,4,5,6}}, loaded = {0};
    assert(device_store_load(&loaded) == ESP_ERR_NOT_FOUND);
    fail_write = true;
    assert(device_store_save(&device) == ESP_FAIL && closes == 1);
    fail_write = false; fail_commit = true;
    assert(device_store_save(&device) == ESP_FAIL && closes == 2);
    assert(device_store_load(&loaded) == ESP_ERR_NOT_FOUND);
    fail_commit = false;
    assert(device_store_save(&device) == ESP_OK && closes == 3);
    assert(device_store_load(&loaded) == ESP_OK);
    assert(!memcmp(&device, &loaded, sizeof(device)));
    memset(&loaded, 0, sizeof(loaded)); // Fresh boot caller, no cached record.
    assert(device_store_load(&loaded) == ESP_OK && !memcmp(&device, &loaded, sizeof(device)));
    disk[0] = 99;
    assert(device_store_load(&loaded) == ESP_ERR_INVALID_STATE);
    assert(disk[0] == 99); // Unsupported data is not silently erased.
    puts("Pairing persistence, fresh load, write/commit failure and version handling: passed");
}
