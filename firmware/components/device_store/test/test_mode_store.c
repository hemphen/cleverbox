// SPDX-License-Identifier: MIT
// Only the NVS boundary is fake. Mode format/writes are production.
#include "device_store.h"
#include "nvs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static const char *keys[] = {"rf_mode", "registry", "wifi", "timezone", "mqtt", "ha_discovery", "ha_monitors"};
static unsigned char disk[7][16];
static size_t sizes[7];
static int open_error, read_key = -1, write_error, commit_error;
static unsigned writes;
static unsigned index_of(const char *key) {
    for (unsigned i=0; i<7; ++i) if (!strcmp(key, keys[i])) return i;
    assert(0); return 0;
}
esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *h) {
    (void)mode; assert(!strcmp(ns,"cleverbox")); *h=1; return open_error;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *n) {
    assert(h==1); unsigned i=index_of(key);
    if ((int)i==read_key) return ESP_FAIL;
    if (!sizes[i]) return ESP_ERR_NVS_NOT_FOUND;
    if (!out) { *n=sizes[i]; return ESP_OK; }
    if (*n<sizes[i]) return ESP_ERR_INVALID_STATE;
    memcpy(out,disk[i],sizes[i]); *n=sizes[i]; return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *data, size_t n) {
    assert(h==1 && !strcmp(key,"rf_mode") && n<=16); ++writes;
    if (write_error) return write_error;
    memcpy(disk[0],data,n); sizes[0]=n; return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { assert(h==1); return commit_error; }
void nvs_close(nvs_handle_t h) { assert(h==1); }
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key) { (void)h; (void)key; assert(0); return ESP_FAIL; }
int main(void) {
    bool enabled=true, stored=true;
    assert(device_mode_load(&enabled,&stored)==ESP_OK && !enabled && !stored && !writes);
    open_error=ESP_FAIL;
    assert(device_mode_load(&enabled,&stored)==ESP_FAIL && !enabled && !stored);
    open_error=ESP_ERR_NVS_NOT_FOUND;
    assert(device_mode_load(&enabled,&stored)==ESP_OK && !enabled && !stored);
    open_error=0;
    for (unsigned i=1; i<7; ++i) {
        memset(sizes,0,sizeof(sizes)); sizes[i]=1; disk[i][0]=255;
        assert(device_mode_load(&enabled,&stored)==ESP_OK && !enabled && !stored);
    }
    memset(sizes,0,sizeof(sizes));
    for (int i=0; i<1; ++i) {
        read_key=i;
        assert(device_mode_load(&enabled,&stored)==ESP_FAIL && !enabled && !stored);
    }
    read_key=-1;
    assert(device_mode_start(&enabled)==ESP_OK && !enabled && writes==1);
    assert(device_mode_load(&enabled,&stored)==ESP_OK && !enabled && stored);
    sizes[1]=7; memset(disk[1],0xA5,7); // unrelated settings are never erased
    assert(device_mode_save(true)==ESP_OK);
    assert(device_mode_load(&enabled,&stored)==ESP_OK && enabled && stored);
    assert(sizes[1]==7 && disk[1][0]==0xA5);
    write_error=ESP_FAIL;
    assert(device_mode_save(false)==ESP_FAIL);
    assert(device_mode_load(&enabled,&stored)==ESP_OK && enabled && stored);
    write_error=0; commit_error=ESP_FAIL;
    assert(device_mode_save(false)==ESP_FAIL); // eager NVS may already contain new bytes
    commit_error=0;
    assert(device_mode_load(&enabled,&stored)==ESP_OK && !enabled && stored);
    read_key=0;
    assert(device_mode_save(true)==ESP_FAIL); // verification failure is not success
    read_key=-1;
    for (unsigned n=1; n<=3; ++n) {
        sizes[0]=n; memset(disk[0],255,n);
        assert(device_mode_load(&enabled,&stored)!=ESP_OK && !enabled && !stored);
        assert(sizes[0]==n && disk[0][0]==255);
    }
    sizes[0]=2; disk[0][0]=1; disk[0][1]=2;
    assert(device_mode_load(&enabled,&stored)!=ESP_OK && !enabled);
    puts("Operating-mode store: fresh defaults, restart, malformed/read failures, eager writes, commit/readback failures and no erasure: passed");
}
