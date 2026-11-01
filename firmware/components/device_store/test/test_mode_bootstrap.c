// SPDX-License-Identifier: MIT
// Production bootstrap with an NVS boundary that can cache a failed write while
// leaving flash absent. Same-value set/commit/readback deliberately does not heal it.
#include "mode_store.h"
#include "nvs.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static uint8_t cached[2], flash[2];
static bool cached_present, flash_present, fail_write=true;
static unsigned scans, writes, erases;
esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *h) { (void)mode; assert(!strcmp(ns,"cleverbox")); *h=1; return ESP_OK; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *n) {
    assert(h==1);
    if (strcmp(key,"rf_mode")) { ++scans; return ESP_ERR_NVS_NOT_FOUND; }
    if (!cached_present) return ESP_ERR_NVS_NOT_FOUND;
    if (!out) { *n=2; return ESP_OK; }
    assert(*n>=2); memcpy(out,cached,2); *n=2; return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *data, size_t n) {
    assert(h==1 && !strcmp(key,"rf_mode") && n==2); ++writes;
    if (cached_present && !memcmp(cached,data,2)) return ESP_OK;
    memcpy(cached,data,2); cached_present=true;
    if (fail_write) return ESP_FAIL;
    memcpy(flash,data,2); flash_present=true; return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { assert(h==1); return ESP_OK; }
void nvs_close(nvs_handle_t h) { assert(h==1); }
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key) {
    assert(h==1 && !strcmp(key,"rf_mode")); ++erases;
    if (fail_write) return ESP_FAIL;
    cached_present=flash_present=false; return ESP_OK;
}
static atomic_int ready, go;
static void *prepare(void *unused) {
    (void)unused; atomic_fetch_add(&ready,1);
    while(!atomic_load(&go)) sched_yield();
    assert(device_mode_prepare_write()==ESP_OK); return NULL;
}
static void *enable(void *unused) {
    (void)unused; atomic_fetch_add(&ready,1);
    while(!atomic_load(&go)) sched_yield();
    assert(device_mode_save(true)==ESP_OK); return NULL;
}
int main(int argc, char **argv) {
    bool enabled=true;
    assert(device_mode_prepare_write()==ESP_ERR_INVALID_STATE);
    assert(device_mode_start(&enabled)==ESP_FAIL && !enabled);
    assert(writes==1 && cached_present && !flash_present && scans==0);
    assert(device_mode_prepare_write()==ESP_FAIL && !flash_present);
    assert(device_mode_start(&enabled)==ESP_FAIL && !enabled && scans==0);
    fail_write=false;
    if (argc==2 && !strcmp(argv[1],"enable-first")) {
        assert(device_mode_save(true)==ESP_OK && flash_present && flash[1]==1);
        unsigned previous=writes;
        assert(device_mode_prepare_write()==ESP_OK && writes==previous && flash[1]==1);
        puts("Explicit enable before bootstrap retry: no delayed fallback overwrite: passed");
    } else if (argc==2 && !strcmp(argv[1],"race")) {
        pthread_t a,b;
        assert(!pthread_create(&a,NULL,prepare,NULL) && !pthread_create(&b,NULL,enable,NULL));
        while(atomic_load(&ready)!=2) sched_yield();
        atomic_store(&go,1); pthread_join(a,NULL); pthread_join(b,NULL);
        assert(flash_present && flash[0]==1 && flash[1]==1);
        puts("Bootstrap retry versus explicit mode save: serialized, explicit enable preserved: passed");
    } else {
        assert(device_mode_prepare_write()==ESP_OK && flash_present && flash[0]==1 && flash[1]==0);
        assert(erases==2 && writes==2 && scans==0);
        assert(device_mode_start(&enabled)==ESP_FAIL && !enabled); // RF error stays latched this boot
        unsigned previous=writes;
        assert(device_mode_prepare_write()==ESP_OK && writes==previous); // no per-setting mode write
        assert(device_mode_save(true)==ESP_OK && flash[1]==1);
        assert(device_mode_prepare_write()==ESP_OK && flash[1]==1);
        puts("Initial mode failure: immutable decision, blocked stores, forced durable retry and no reclassification: passed");
    }
}
