// SPDX-License-Identifier: MIT
#include "device_store.h"
#include "nvs.h"
#include <string.h>

esp_err_t store_load(const char *key, void *data, size_t *size)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open("cleverbox", NVS_READONLY, &h);
    if (e != ESP_OK) return e;
    e = nvs_get_blob(h, key, data, size);
    nvs_close(h);
    return e;
}

esp_err_t store_save(const char *key, const void *data, size_t size)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open("cleverbox", NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_blob(h, key, data, size);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

esp_err_t store_erase(const char *key)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open("cleverbox", NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_erase_key(h, key);
    if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

bool radiator_paired(const radiator_record *r)
{
    return r->controller[0] || r->controller[1] || r->controller[2] || r->controller[3];
}

bool radiator_name_valid(const char *name)
{
    if (!name || !*name || strlen(name) > RADIATOR_NAME_BYTES) return false;
    for (const char *c = name; *c; ++c)
        if ((unsigned char)*c < 32 || *c == 127) return false;
    return true;
}

enum { HEADER = 7, RECORD = 4 + RADIATOR_NAME_BYTES + 1 + 8 };
static uint8_t buffer[HEADER + RECORD * CLEVERBOX_RADIATOR_CAPACITY];

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put32(uint8_t *p, uint32_t n)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(n >> (8 * i));
}

esp_err_t registry_load(radiator_registry *r)
{
    memset(r, 0, sizeof(*r));
    r->next_id = 1;
    size_t size = sizeof(buffer);
    esp_err_t e = store_load("registry", buffer, &size);
    if (e == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (e != ESP_OK) return e;
    size_t count = buffer[1] | (size_t)buffer[2] << 8;
    if (size < HEADER || buffer[0] != 2 || count > CLEVERBOX_RADIATOR_CAPACITY ||
        size != HEADER + count * RECORD) return ESP_ERR_INVALID_STATE;
    r->next_id = get32(buffer + 3);
    for (size_t i = 0; i < count; ++i) {
        const uint8_t *b = buffer + HEADER + i * RECORD;
        radiator_record *d = &r->records[i];
        d->id = get32(b);
        memcpy(d->name, b + 4, sizeof(d->name));
        d->name[RADIATOR_NAME_BYTES] = 0;
        memcpy(d->controller, b + 4 + sizeof(d->name), 4);
        memcpy(d->radiator, b + 8 + sizeof(d->name), 4);
        if (!d->id || d->id >= r->next_id) { memset(r, 0, sizeof(*r)); r->next_id = 1; return ESP_ERR_INVALID_STATE; }
    }
    r->count = count;
    return ESP_OK;
}

esp_err_t registry_save(const radiator_registry *r)
{
    buffer[0] = 2;
    buffer[1] = (uint8_t)r->count;
    buffer[2] = (uint8_t)(r->count >> 8);
    put32(buffer + 3, r->next_id);
    for (size_t i = 0; i < r->count; ++i) {
        uint8_t *b = buffer + HEADER + i * RECORD;
        const radiator_record *d = &r->records[i];
        put32(b, d->id);
        memcpy(b + 4, d->name, sizeof(d->name));
        memcpy(b + 4 + sizeof(d->name), d->controller, 4);
        memcpy(b + 8 + sizeof(d->name), d->radiator, 4);
    }
    return store_save("registry", buffer, HEADER + r->count * RECORD);
}
