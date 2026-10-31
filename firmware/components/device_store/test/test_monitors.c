// SPDX-License-Identifier: MIT
#include "device_store.h"
#include "nvs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint8_t visible[MONITOR_STORAGE_BYTES], durable[MONITOR_STORAGE_BYTES], deferred[MONITOR_STORAGE_BYTES];
static size_t size, durable_size, deferred_size;
static unsigned opens, reads, sets, commits, fail_open, fail_read, fail_set, fail_commit;
static bool mismatch, defer, eager;
static int gate;
esp_err_t device_mode_prepare_write(void) { return gate; }
static void reset(void) { opens=reads=sets=commits=fail_open=fail_read=fail_set=fail_commit=0; mismatch=defer=eager=false; gate=0; }
static void reboot(void) {
    if (deferred_size) { memcpy(durable,deferred,deferred_size); durable_size=deferred_size; deferred_size=0; }
    size=durable_size; memcpy(visible,durable,size);
}
esp_err_t nvs_open(const char *ns,int mode,nvs_handle_t *h) {
    assert(!strcmp(ns,"cleverbox")); (void)mode; if (++opens==fail_open) return ESP_FAIL; *h=1; return ESP_OK;
}
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *out,size_t *n) {
    (void)h; assert(!strcmp(key,"ha_monitors")); if (++reads==fail_read) return ESP_FAIL;
    if (!size) return ESP_ERR_NVS_NOT_FOUND;
    if (*n<size) return ESP_ERR_INVALID_STATE;
    memcpy(out,visible,size); *n=size; if (mismatch) ((uint8_t *)out)[0]^=1; return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h,const char *key,const void *in,size_t n) {
    (void)h; assert(!strcmp(key,"ha_monitors") && n<=sizeof(visible));
    if (n==size && !memcmp(in,visible,n)) return ESP_OK;
    if (++sets==fail_set) {
        if (defer) { memcpy(deferred,in,n); deferred_size=n; }
        if (eager) { memcpy(durable,in,n); durable_size=n; memcpy(visible,in,n); size=n; }
        return ESP_FAIL;
    }
    memcpy(visible,in,n); size=n; memcpy(durable,in,n); durable_size=n; deferred_size=0; return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ++commits==fail_commit ? ESP_FAIL : ESP_OK; }
int main(void) {
    monitor_registry r={0}, actual;
    assert(device_monitors_load(&actual)==ESP_OK && !actual.count);
    r.count=1; memcpy(r.records[0].address,"\xF1\x01\x02\x03",4); strcpy(r.records[0].name,"Test");
    gate=ESP_FAIL; assert(device_monitors_save(&r)==ESP_FAIL && !sets && !size); reset();
    assert(device_monitors_save(&r)==ESP_OK); reboot(); assert(device_monitors_load(&actual)==ESP_OK && actual.count==1);
    for (unsigned step=0;step<6;++step) {
        reset();
        switch(step) {
            case 0: fail_open=1; break;
            case 1: fail_set=1; defer=true; break;
            case 2: fail_set=1; eager=true; break;
            case 3: fail_commit=1; break;
            case 4: fail_open=2; break;
            case 5: fail_read=1; break;
        }
        strcpy(r.records[0].name,"Changed"); assert(device_monitors_save(&r)!=ESP_OK);
        reset(); assert(device_monitors_save(&r)==ESP_OK && sets==1); reboot();
        assert(device_monitors_load(&actual)==ESP_OK && !strcmp(actual.records[0].name,"Changed"));
    }
    reset(); mismatch=true; assert(device_monitors_save(&r)!=ESP_OK); reset();
    assert(device_monitors_save(&r)==ESP_OK); visible[0]=99; assert(device_monitors_load(&actual)!=ESP_OK);
    reboot(); assert(device_monitors_load(&actual)==ESP_OK);
    r.count=0; assert(device_monitors_save(&r)==ESP_OK); reboot(); assert(device_monitors_load(&actual)==ESP_OK && !actual.count);
    r.count=RADIO_MONITOR_CAPACITY+1; assert(device_monitors_save(&r)!=ESP_OK);
    puts("Monitor store: mode gate, failed/deferred/eager writes, commit/readback errors, forced retry, reboot, corruption and empty list: passed");
}
