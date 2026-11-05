// SPDX-License-Identifier: MIT
// Uses the host's file-backed NVS (host/storage.c).
#define _POSIX_C_SOURCE 200809L
#include "device_store.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    char path[] = "/tmp/cleverbox-store-XXXXXX", file[64];
    int fd = mkstemp(path); assert(fd >= 0); close(fd); unlink(path);
    setenv("CLEVERBOX_SIM_STORAGE", path, 1);
    snprintf(file, sizeof(file), "%s.registry", path);

    radiator_registry r, loaded;
    assert(registry_load(&r) == ESP_OK && r.count == 0 && r.next_id == 1);
    // One paired and one monitored (all-zero controller) record.
    r.records[0] = (radiator_record){.id = 1, .name = "Living room", .controller = {0xf1, 1, 2, 3}, .radiator = {0xf1, 4, 5, 6}};
    r.records[1] = (radiator_record){.id = 2, .name = "Hall", .radiator = {0xf1, 4, 5, 7}};
    r.records[0].has_report = true; // RAM-only fields are not stored.
    r.count = 2; r.next_id = 3;
    assert(registry_save(&r) == ESP_OK);
    assert(registry_load(&loaded) == ESP_OK && loaded.count == 2 && loaded.next_id == 3);
    assert(radiator_paired(&loaded.records[0]) && !radiator_paired(&loaded.records[1]));
    assert(!strcmp(loaded.records[1].name, "Hall") && !loaded.records[0].has_report);
    assert(!memcmp(loaded.records[0].controller, r.records[0].controller, 4));

    // Exact v2 layout: header, then ID, name, controller, radiator.
    unsigned char bytes[256];
    FILE *f = fopen(file, "rb"); size_t n = fread(bytes, 1, sizeof(bytes), f); fclose(f);
    assert(n == 7 + 2 * 45 && bytes[0] == 2 && bytes[1] == 2 && bytes[3] == 3);
    assert(!memcmp(bytes + 7 + 37, "\xf1\x01\x02\x03\xf1\x04\x05\x06", 8));

    // Unknown version or a size mismatch loads nothing and reports an error.
    bytes[0] = 3; f = fopen(file, "wb"); fwrite(bytes, 1, n, f); fclose(f);
    assert(registry_load(&loaded) == ESP_ERR_INVALID_STATE && loaded.count == 0 && loaded.next_id == 1);
    bytes[0] = 2; f = fopen(file, "wb"); fwrite(bytes, 1, n - 1, f); fclose(f);
    assert(registry_load(&loaded) == ESP_ERR_INVALID_STATE && loaded.count == 0);

    assert(radiator_name_valid("Bedroom") && !radiator_name_valid("") && !radiator_name_valid(NULL));
    assert(!radiator_name_valid("tab\there") && !radiator_name_valid("123456789012345678901234567890123"));
    assert(store_erase("registry") == ESP_OK && store_erase("registry") == ESP_OK);
    assert(registry_load(&loaded) == ESP_OK && loaded.count == 0);
    puts("Registry: paired and monitored records, v2 layout, invalid records and erase: passed");
}
