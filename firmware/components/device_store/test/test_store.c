// SPDX-License-Identifier: MIT
#include "device_store.h"
#include "nvs.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// Fake only the NVS boundary. Writes are deliberately eager, as ESP-IDF NVS
// may write before commit: closing a handle must not be mistaken for rollback.
static unsigned char disk[2][80];
static size_t sizes[2];
static bool fail_write;
static unsigned fail_commits;
static unsigned key_index(const char *key)
{
    assert(!strcmp(key, "pairing") || !strcmp(key, "control_state"));
    return !strcmp(key, "pairing") ? 0 : 1;
}
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *out)
{
    assert(!strcmp(name, "cleverbox")); *out = 1;
    return mode == NVS_READONLY && !sizes[0] && !sizes[1] ? ESP_ERR_NVS_NOT_FOUND : ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *size)
{
    assert(h == 1);
    unsigned i = key_index(key);
    if (!sizes[i]) return ESP_ERR_NVS_NOT_FOUND;
    if (*size < sizes[i]) return ESP_ERR_INVALID_STATE;
    memcpy(out, disk[i], sizes[i]); *size = sizes[i]; return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *in, size_t size)
{
    assert(h == 1 && size <= sizeof(disk[0]));
    if (fail_write) return ESP_FAIL;
    unsigned i = key_index(key);
    memcpy(disk[i], in, size); sizes[i] = size; return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h)
{
    assert(h == 1);
    if (fail_commits) { --fail_commits; return ESP_FAIL; }
    return ESP_OK;
}
void nvs_close(nvs_handle_t h) { assert(h == 1); }

int main(void)
{
    paired_device device = {{0xf1,1,2,3}, {0xf1,4,5,6}}, loaded = {0};
    assert(device_store_load(&loaded) == ESP_ERR_NOT_FOUND);
    fail_write = true;
    assert(device_store_save(&device) == ESP_FAIL);
    fail_write = false;
    assert(device_store_save(&device) == ESP_OK);
    assert(sizes[0] == 9 && disk[0][0] == 1);
    assert(device_store_load(&loaded) == ESP_OK && !memcmp(&device, &loaded, sizeof(device)));
    rf_frame frame = {0}, out = {0}, other;
    assert(device_store_load_state(&device, &out) == ESP_ERR_NOT_FOUND);
    assert(rf_parse_hex("38F1010203C6F10405064500004B02A83B004C00008E000084CC00000000972030049402BA00E100641919DA000000005B0078B20C0000A20F60D9", frame.bytes, sizeof(frame.bytes), &frame.size));
    fail_write = true;
    assert(device_store_save_state(&device, &frame) == ESP_FAIL);
    fail_write = false;
    assert(device_store_load_state(&device, &out) == ESP_ERR_NOT_FOUND);
    fail_commits = 1;
    assert(device_store_save_state(&device, &frame) == ESP_FAIL);
    assert(device_store_load_state(&device, &out) == ESP_ERR_NOT_FOUND);
    assert(device_store_save_state(&device, &frame) == ESP_OK);
    assert(device_store_load_state(&device, &out) == ESP_OK && !memcmp(&frame, &out, sizeof(frame)));
    paired_device wrong = device; wrong.controller[3] ^= 1;
    assert(device_store_load_state(&wrong, &out) == ESP_ERR_INVALID_STATE);
    wrong = device; wrong.radiator[3] ^= 1;
    assert(device_store_load_state(&wrong, &out) == ESP_ERR_INVALID_STATE);
    assert(device_store_save_state(&wrong, &frame) == ESP_ERR_INVALID_ARG);
    assert(rf_set_target(&other, &frame, RF_ECO, 1500));
    fail_write = true;
    assert(device_store_save_state(&device, &other) == ESP_FAIL);
    fail_write = false;
    assert(device_store_load_state(&device, &out) == ESP_OK && !memcmp(&frame, &out, sizeof(frame)));
    fail_commits = 1;
    assert(device_store_save_state(&device, &other) == ESP_FAIL);
    assert(device_store_load_state(&device, &out) == ESP_OK && !memcmp(&frame, &out, sizeof(frame)));
    assert(device_store_save_state(&device, &other) == ESP_OK); // Explicit retry, no RF/pairing.
    assert(device_store_load_state(&device, &out) == ESP_OK && !memcmp(&other, &out, sizeof(other)));
    fail_commits = 1;
    assert(device_store_save_state(&device, NULL) == ESP_FAIL);
    assert(device_store_load_state(&device, &out) == ESP_OK && !memcmp(&other, &out, sizeof(other)));
    frame.bytes[55] ^= 1;
    assert(device_store_save_state(&device, &frame) == ESP_ERR_INVALID_ARG);
    disk[1][0] = 99;
    assert(device_store_load_state(&device, &out) == ESP_ERR_INVALID_STATE && disk[1][0] == 99);
    assert(device_store_load(&loaded) == ESP_OK && !memcmp(&device, &loaded, sizeof(device)));
    assert(device_store_save_state(&device, NULL) == ESP_OK);
    assert(device_store_load_state(&device, &out) == ESP_ERR_NOT_FOUND);
    assert(device_store_load(&loaded) == ESP_OK && !memcmp(&device, &loaded, sizeof(device)));
    disk[0][0] = 99;
    assert(device_store_load(&loaded) == ESP_ERR_INVALID_STATE && disk[0][0] == 99);
    puts("Pairing compatibility, state identity/CRC validation, eager write/commit failure rollback, retry and clear: passed");
}
