// SPDX-License-Identifier: MIT
// NVS cache/deferred-error boundary; production MQTT store, no shadow policy.
#include "mqtt_bridge.h"
#include "nvs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned char visible[MQTT_RECORD_BYTES], durable[MQTT_RECORD_BYTES], deferred[MQTT_RECORD_BYTES];
static bool present, durable_present, has_deferred;
static unsigned sets, erases, commits, opens, reads, fail_set, fail_erase, fail_commit, fail_open, fail_read;
static bool defer_error, mutate_error, mismatch;
static void reset_faults(void) {
    sets=erases=commits=opens=reads=fail_set=fail_erase=fail_commit=fail_open=fail_read=0;
    defer_error=mutate_error=mismatch=false;
}
static void reboot(void) {
    if (has_deferred) { memcpy(durable,deferred,sizeof(durable)); durable_present=true; has_deferred=false; }
    present=durable_present; memcpy(visible,durable,sizeof(visible));
}
esp_err_t nvs_open(const char *ns,int mode,nvs_handle_t *h) {
    assert(!strcmp(ns,"cleverbox")); (void)mode;
    if (++opens==fail_open) return ESP_FAIL;
    *h=1; return ESP_OK;
}
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *out,size_t *size) {
    (void)h; assert(!strcmp(key,"mqtt"));
    if (++reads==fail_read) return ESP_FAIL;
    if (!present) return ESP_ERR_NVS_NOT_FOUND;
    assert(*size>=sizeof(visible)); memcpy(out,visible,sizeof(visible)); *size=sizeof(visible);
    if (mismatch) ((unsigned char *)out)[0]^=1;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h,const char *key,const void *in,size_t size) {
    (void)h; assert(!strcmp(key,"mqtt") && size==sizeof(visible));
    if (present && !memcmp(visible,in,size)) return ESP_OK; // NVS same-value optimization.
    if (++sets==fail_set) {
        if (defer_error) { memcpy(deferred,in,size); has_deferred=true; }
        if (mutate_error) { memcpy(visible,in,size); memcpy(durable,in,size); present=durable_present=true; has_deferred=false; }
        return ESP_FAIL;
    }
    memcpy(visible,in,size); memcpy(durable,in,size); present=durable_present=true; has_deferred=false;
    return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t h,const char *key) {
    (void)h; assert(!strcmp(key,"mqtt"));
    if (++erases==fail_erase) return ESP_FAIL;
    if (!present) return ESP_ERR_NVS_NOT_FOUND;
    present=durable_present=has_deferred=false; return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ++commits==fail_commit ? ESP_FAIL : ESP_OK; }
static mqtt_settings original, replacement, empty;
static void seed(void) {
    reset_faults(); assert(mqtt_store_save(&original)==ESP_OK); reboot(); reset_faults();
}
static void verify(const mqtt_settings *expected) {
    mqtt_settings actual; assert(mqtt_store_load(&actual)==ESP_OK);
    assert(actual.configured==expected->configured && actual.enabled==expected->enabled && actual.port==expected->port);
    assert(!strcmp(actual.host,expected->host) && !strcmp(actual.username,expected->username) && !strcmp(actual.password,expected->password));
}
int main(void) {
    original.configured=original.enabled=true; original.port=1883; strcpy(original.host,"broker.local");
    // Test secret is generated in memory, never credentials for a real broker.
    strcpy(original.username,"bench"); for (unsigned i=0;i<16;++i) original.password[i]=(char)('a'+i);
    replacement=original; strcpy(replacement.host,"replacement.local");
    assert(mqtt_store_load(&empty)==ESP_ERR_NVS_NOT_FOUND);
    seed(); fail_set=1; defer_error=true;
    assert(mqtt_store_save(&replacement)==ESP_FAIL && has_deferred);
    // Erase left cached absence; explicit original retry must really rewrite.
    reset_faults(); assert(mqtt_store_save(&original)==ESP_OK); reboot(); verify(&original);
    // A failed write with a visible mutation, retried across every failure boundary.
    for (unsigned step=0;step<7;++step) {
        seed(); fail_set=1; mutate_error=true; assert(mqtt_store_save(&replacement)==ESP_FAIL);
        reset_faults();
        switch (step) {
            case 0: fail_open=1; break;
            case 1: fail_erase=1; break;
            case 2: fail_commit=1; break;
            case 3: fail_set=1; break;
            case 4: fail_commit=2; break;
            case 5: fail_open=2; break;
            case 6: fail_read=1; break;
        }
        assert(mqtt_store_save(&original)!=ESP_OK);
        reset_faults(); assert(mqtt_store_save(&original)==ESP_OK); reboot(); verify(&original);
    }
    seed(); fail_set=1; defer_error=true; assert(mqtt_store_save(&replacement)==ESP_FAIL);
    reset_faults(); assert(mqtt_store_save(&empty)==ESP_OK); reboot(); verify(&empty); // durable forget tombstone.
    seed(); mismatch=true; assert(mqtt_store_save(&replacement)!=ESP_OK);
    reset_faults(); assert(mqtt_store_save(&original)==ESP_OK);
    visible[0]=99; assert(mqtt_store_load(&empty)==ESP_ERR_INVALID_STATE);
    seed(); visible[4+MQTT_HOST_BYTES]=42; assert(mqtt_store_load(&empty)==ESP_ERR_INVALID_STATE);
    seed(); visible[MQTT_RECORD_BYTES-1]=42; assert(mqtt_store_load(&empty)==ESP_ERR_INVALID_STATE);
    replacement.host[0]=0; assert(!mqtt_settings_valid(&replacement));
    puts("MQTT store: validation, deferred write/retry/restart, forced saves, forget tombstone and failure boundaries: passed");
}
