// SPDX-License-Identifier: MIT
#include "device_store.h"
#include "nvs.h"
#include <string.h>
// Single radio worker owner. Generation forces same-value retries after an
// uncertain write. No report data is persisted, and unknown records are kept.
static uint8_t data[MONITOR_STORAGE_BYTES], verify[MONITOR_STORAGE_BYTES];
static uint32_t generation;
static bool valid(const monitor_registry *r) {
    if (r->count > RADIO_MONITOR_CAPACITY) return false;
    for (size_t i=0; i<r->count; ++i) {
        const monitor_selection *d=&r->records[i];
        if (!d->name[0] || !memchr(d->name,0,sizeof(d->name))) return false;
        for (const char *c=d->name; *c; ++c) if ((unsigned char)*c<32 || *c==127) return false;
        for (size_t j=0; j<i; ++j) if (!memcmp(d->address,r->records[j].address,4)) return false;
    }
    return true;
}
esp_err_t device_monitors_load(monitor_registry *r) {
    memset(r,0,sizeof(*r)); generation=0;
    nvs_handle_t h; size_t size=sizeof(data);
    esp_err_t e=nvs_open("cleverbox",NVS_READONLY,&h);
    if (e==ESP_OK) { e=nvs_get_blob(h,"ha_monitors",data,&size); nvs_close(h); }
    if (e==ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (e!=ESP_OK) return e;
    if (size<7 || data[0]!=1) return ESP_ERR_INVALID_STATE;
    size_t count=data[1] | (size_t)data[2]<<8;
    if (count>RADIO_MONITOR_CAPACITY) return DEVICE_STORE_ERR_CAPACITY;
    if (size!=7+count*(4+RADIATOR_NAME_BYTES+1)) return ESP_ERR_INVALID_STATE;
    for (unsigned b=0;b<4;++b) generation|=(uint32_t)data[3+b]<<(8*b);
    r->count=count;
    for (size_t i=0;i<count;++i) {
        const uint8_t *v=data+7+i*(4+RADIATOR_NAME_BYTES+1);
        memcpy(r->records[i].address,v,4); memcpy(r->records[i].name,v+4,RADIATOR_NAME_BYTES+1);
    }
    if (!valid(r)) { r->count=0; return ESP_ERR_INVALID_STATE; }
    return ESP_OK;
}
esp_err_t device_monitors_save(const monitor_registry *r) {
    if (!valid(r) || generation==UINT32_MAX) return ESP_ERR_INVALID_STATE;
    esp_err_t e=device_mode_prepare_write(); if (e!=ESP_OK) return e;
    ++generation; memset(data,0,sizeof(data)); data[0]=1; data[1]=r->count; data[2]=r->count>>8;
    for (unsigned b=0;b<4;++b) data[3+b]=generation>>(8*b);
    size_t size=7+r->count*(4+RADIATOR_NAME_BYTES+1);
    for (size_t i=0;i<r->count;++i) {
        uint8_t *v=data+7+i*(4+RADIATOR_NAME_BYTES+1);
        memcpy(v,r->records[i].address,4); memcpy(v+4,r->records[i].name,RADIATOR_NAME_BYTES+1);
    }
    nvs_handle_t h; e=nvs_open("cleverbox",NVS_READWRITE,&h);
    if (e!=ESP_OK) return e;
    e=nvs_set_blob(h,"ha_monitors",data,size); if (e==ESP_OK) e=nvs_commit(h); nvs_close(h);
    if (e==ESP_OK) {
        e=nvs_open("cleverbox",NVS_READONLY,&h);
        if (e==ESP_OK) { size_t n=sizeof(verify); e=nvs_get_blob(h,"ha_monitors",verify,&n); nvs_close(h);
            if (e==ESP_OK && (n!=size || memcmp(data,verify,size))) e=ESP_FAIL; }
    }
    return e;
}
