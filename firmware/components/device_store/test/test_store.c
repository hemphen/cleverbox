// SPDX-License-Identifier: MIT
#include "device_store.h"
#include "nvs.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// Fake only the NVS boundary. Writes are deliberately eager, as ESP-IDF NVS
// may write before commit: closing a handle must not be mistaken for rollback.
static unsigned char disk[1][REGISTRY_STORAGE_BYTES + REGISTRY_RECORD_BYTES];
static size_t sizes[1];
static bool fail_write;
static unsigned fail_commits;
static unsigned key_index(const char *key)
{
    assert(!strcmp(key, "registry")); return 0;
}
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *out)
{
    assert(!strcmp(name, "cleverbox")); *out = 1;
    return mode == NVS_READONLY && !sizes[0] ? ESP_ERR_NVS_NOT_FOUND : ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *size)
{
    assert(h == 1);
    unsigned i = key_index(key);
    if (!sizes[i]) return ESP_ERR_NVS_NOT_FOUND;
    if (!out) { *size = sizes[i]; return ESP_OK; }
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
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key) { assert(h == 1); sizes[key_index(key)] = 0; return ESP_OK; }
void nvs_close(nvs_handle_t h) { assert(h == 1); }

int main(void)
{
    radiator_registry r = {0}, loaded_registry = {0};
    assert(device_registry_load(&r) == ESP_OK && !r.count && r.next_id == 1);
    radiator_record first = {.id=1, .name="Radiator", .pair={{0xf1,1,2,3},{0xf1,4,5,6}}};
    fail_write = true;
    assert(device_registry_save_record(&r,&first,0) == ESP_FAIL && !sizes[0]);
    fail_write = false; fail_commits = 1;
    assert(device_registry_save_record(&r,&first,0) == ESP_FAIL && !sizes[0]);
    assert(device_registry_save_record(&r,&first,0) == ESP_OK);
    assert(device_registry_load(&r) == ESP_OK && r.count==1 && r.next_id==2);
    assert(sizes[0]==REGISTRY_HEADER_BYTES+REGISTRY_RECORD_BYTES && disk[0][0]==2);
    // The generated controller profile rejects a zero controller identity suffix.
    // The registry must reject it both before saving and when loading stored bytes.
    radiator_record invalid = r.records[0];
    memset(invalid.pair.controller + 1, 0, 3);
    assert(device_registry_save_record(&r, &invalid, 0) == ESP_ERR_INVALID_ARG);
    assert(device_registry_load(&loaded_registry) == ESP_OK && loaded_registry.count == 1);
    const size_t controller_offset = REGISTRY_HEADER_BYTES + 4 + RADIATOR_NAME_BYTES + 1;
    memset(disk[0] + controller_offset + 1, 0, 3);
    assert(device_registry_load(&loaded_registry) == ESP_ERR_INVALID_STATE && !loaded_registry.count);
    assert(disk[0][controller_offset + 1] == 0 && disk[0][controller_offset + 2] == 0 &&
           disk[0][controller_offset + 3] == 0); // Invalid NVS data was not modified.
    memcpy(disk[0] + controller_offset + 1, first.pair.controller + 1, 3);
    assert(device_registry_load(&loaded_registry) == ESP_OK && loaded_registry.count == 1);
    // Full-capacity generated records; no path is special to eight or to two.
    while (r.count < CLEVERBOX_RADIATOR_CAPACITY) {
        radiator_record *d = &r.records[r.count++]; *d = r.records[0];
        d->id = r.next_id++; d->pair.radiator[1] = 0x20; d->pair.radiator[2] = (uint8_t)(d->id >> 8); d->pair.radiator[3] = (uint8_t)(d->id + 10);
    }
    assert(device_registry_save(&r) == ESP_OK);
    assert(device_registry_load(&loaded_registry) == ESP_OK && loaded_registry.count == CLEVERBOX_RADIATOR_CAPACITY);
    radiator_record changed = r.records[0]; strcpy(changed.name, "Bedroom");
    fail_write = true;
    assert(device_registry_save_record(&r, &changed, 0) == ESP_FAIL); fail_write = false;
    assert(device_registry_load(&loaded_registry) == ESP_OK && strcmp(loaded_registry.records[0].name, "Bedroom"));
    fail_commits = 1;
    assert(device_registry_save_record(&r, &changed, 0) == ESP_FAIL);
    assert(device_registry_load(&loaded_registry) == ESP_OK && strcmp(loaded_registry.records[0].name, "Bedroom"));
    assert(device_registry_save_record(&r, &changed, 0) == ESP_OK);
    assert(device_registry_load(&loaded_registry) == ESP_OK && !strcmp(loaded_registry.records[0].name, "Bedroom"));
    changed.id = r.next_id; changed.pair.radiator[3] = 99;
    assert(device_registry_save_record(&r, &changed, 0) == ESP_ERR_INVALID_STATE); // Full.
    changed.id = r.next_id; changed.pair = r.records[0].pair;
    assert(device_registry_save_record(&r, &changed, 0) != ESP_OK); // Duplicate.
    // A valid larger-capacity blob cannot be loaded at this capacity. It remains
    // byte-for-byte present, and a save cannot overwrite an oversized record.
    size_t original_size = sizes[0];
    sizes[0] += REGISTRY_RECORD_BYTES; disk[0][1] = CLEVERBOX_RADIATOR_CAPACITY + 1;
    assert(device_registry_load(&loaded_registry) == DEVICE_STORE_ERR_CAPACITY && !loaded_registry.count);
    assert(device_registry_save(&r) != ESP_OK && sizes[0] == original_size + REGISTRY_RECORD_BYTES);
    sizes[0] = original_size; disk[0][1] = CLEVERBOX_RADIATOR_CAPACITY;
    disk[0][0] = 99;
    assert(device_registry_load(&loaded_registry) == ESP_ERR_INVALID_STATE && !loaded_registry.count && disk[0][0] == 99);
    disk[0][0] = 2;
    r.count = 0;
    assert(device_registry_save(&r) == ESP_OK);
    assert(device_registry_load(&loaded_registry) == ESP_OK && loaded_registry.count == 0);
    assert(loaded_registry.next_id == r.next_id);
    // Retired and unknown formats fail without writes or implicit migration.
    disk[0][0]=1;
    assert(device_registry_load(&loaded_registry)==ESP_ERR_INVALID_STATE && !loaded_registry.count);
    assert(disk[0][0]==1);
    puts("Registry v2: identities/names, capacity, unknown format rejection, failed writes/commit rollback and authoritative removal: passed");
}
