// SPDX-License-Identifier: MIT
#include "device_store.h"
#include "nvs.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// Fake only the NVS boundary. Writes are deliberately eager, as ESP-IDF NVS
// may write before commit: closing a handle must not be mistaken for rollback.
static unsigned char disk[3][REGISTRY_STORAGE_BYTES + REGISTRY_RECORD_BYTES];
static size_t sizes[3];
static bool fail_write;
static unsigned fail_commits;
static unsigned key_index(const char *key)
{
    assert(!strcmp(key, "pairing") || !strcmp(key, "control_state") || !strcmp(key, "registry"));
    return !strcmp(key, "pairing") ? 0 : !strcmp(key, "registry") ? 2 : 1;
}
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *out)
{
    assert(!strcmp(name, "cleverbox")); *out = 1;
    return mode == NVS_READONLY && !sizes[0] && !sizes[1] && !sizes[2] ? ESP_ERR_NVS_NOT_FOUND : ESP_OK;
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
    const uint8_t legacy[] = {1,0xf1,1,2,3,0xf1,4,5,6};
    rf_frame frame = {0};
    assert(rf_parse_hex("38F1010203C6F10405064500004B02A83B004C00008E000084CC00000000972030049402BA00E100641919DA000000005B0078B20C0000A20F60D9", frame.bytes, sizeof(frame.bytes), &frame.size));
    radiator_registry empty = {0};
    assert(device_registry_load(&empty) == ESP_OK && empty.count == 0);
    memcpy(disk[0], legacy, sizeof(legacy)); sizes[0] = sizeof(legacy);
    // Migration, eager-write rollback, retry, and authoritative empty registry.
    radiator_registry r = {0}, loaded_registry = {0};
    fail_write = true;
    assert(device_registry_load(&r) == ESP_FAIL && sizes[0] == 9 && !sizes[2]);
    fail_write = false; fail_commits = 1;
    assert(device_registry_load(&r) == ESP_FAIL && sizes[0] == 9 && !sizes[2]);
    assert(device_registry_load(&r) == ESP_OK && r.count == 1 && r.records[0].id == 1);
    assert(!r.records[0].has_saved_template && r.next_id == 2);
    assert(device_registry_load(&loaded_registry) == ESP_OK && loaded_registry.count == 1);
    sizes[2] = 0;
    memcpy(disk[1], legacy, sizeof(legacy)); memcpy(disk[1] + 9, frame.bytes, frame.size); sizes[1] = 68;
    assert(device_registry_load(&r) == ESP_OK && r.records[0].has_saved_template);
    assert(!memcmp(r.records[0].saved_template, frame.bytes, CONTROLLER_STATE_BYTES));
    // Full-capacity generated records; no path is special to eight or to two.
    while (r.count < CLEVERBOX_RADIATOR_CAPACITY) {
        radiator_record *d = &r.records[r.count++]; *d = r.records[0];
        d->id = r.next_id++; d->has_saved_template = false; d->pair.radiator[1] = 0x20; d->pair.radiator[2] = (uint8_t)(d->id >> 8); d->pair.radiator[3] = (uint8_t)(d->id + 10);
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
    changed.id = r.next_id; changed.pair.radiator[3] = 99; changed.has_saved_template = false;
    assert(device_registry_save_record(&r, &changed, 0) == ESP_ERR_INVALID_STATE); // Full.
    changed.id = r.next_id; changed.pair = r.records[0].pair;
    assert(device_registry_save_record(&r, &changed, 0) != ESP_OK); // Duplicate.
    // A valid larger-capacity blob cannot be loaded at this capacity. It remains
    // byte-for-byte present, and a save cannot overwrite an oversized record.
    size_t original_size = sizes[2];
    sizes[2] += REGISTRY_RECORD_BYTES; disk[2][1] = CLEVERBOX_RADIATOR_CAPACITY + 1;
    assert(device_registry_load(&loaded_registry) == DEVICE_STORE_ERR_CAPACITY && !loaded_registry.count);
    assert(device_registry_save(&r) != ESP_OK && sizes[2] == original_size + REGISTRY_RECORD_BYTES);
    sizes[2] = original_size; disk[2][1] = CLEVERBOX_RADIATOR_CAPACITY;
    disk[2][0] = 99;
    assert(device_registry_load(&loaded_registry) == ESP_ERR_INVALID_STATE && !loaded_registry.count && disk[2][0] == 99);
    disk[2][0] = 1;
    r.count = 0;
    assert(device_registry_save(&r) == ESP_OK);
    assert(sizes[0] == 9 && sizes[1] == 68); // Legacy setup retained, but never resurrected.
    assert(device_registry_load(&loaded_registry) == ESP_OK && loaded_registry.count == 0);
    assert(loaded_registry.next_id == r.next_id);
    // Unknown/corrupt optional legacy state retains usable pair and can be
    // repaired explicitly; unknown pairing never starts a fresh registry.
    sizes[2] = 0; disk[1][0] = 99;
    assert(device_registry_load(&loaded_registry) == ESP_ERR_INVALID_STATE && loaded_registry.count == 1);
    assert(loaded_registry.records[0].state_load_error && !loaded_registry.records[0].has_saved_template);
    assert(!sizes[2] && disk[1][0] == 99);
    disk[1][0] = 1; disk[1][67] ^= 1;
    assert(device_registry_load(&loaded_registry) == ESP_ERR_INVALID_STATE && !sizes[2]);
    disk[1][67] ^= 1; disk[1][4] ^= 1;
    assert(device_registry_load(&loaded_registry) == ESP_ERR_INVALID_STATE && !sizes[2]);
    disk[1][4] ^= 1; disk[0][0] = 99;
    assert(device_registry_load(&loaded_registry) == ESP_ERR_INVALID_STATE && !loaded_registry.count && !sizes[2]);
    puts("Legacy/state validation, migration retry/rollback, registry capacity, identity/name/state retention, authoritative removal: passed");
}
