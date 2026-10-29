// SPDX-License-Identifier: MIT
#include "device_time.h"
#include "nvs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static int64_t now_ms = 1000;
int64_t esp_timer_get_time(void) { return now_ms * 1000; }
static int locked, writes, set_error, storage_error;
static unsigned char saved[128]; static size_t saved_size;
void time_platform_lock(void) { assert(!locked++); }
void time_platform_unlock(void) { assert(locked-- == 1); }
esp_err_t time_platform_set_utc(int64_t seconds) { assert(locked && seconds >= DEVICE_TIME_MIN); return set_error; }
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *h) {
    assert(!strcmp(name, "cleverbox")); (void)mode; *h = 1; return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *n) {
    (void)h; assert(!strcmp(key, "timezone"));
    if (!saved_size) return ESP_ERR_NVS_NOT_FOUND;
    if (*n < saved_size) return ESP_ERR_INVALID_STATE;
    memcpy(out, saved, saved_size); *n = saved_size; return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *in, size_t n) {
    (void)h; assert(!strcmp(key, "timezone") && n <= sizeof(saved)); ++writes;
    memcpy(saved, in, n); saved_size = n;
    return storage_error; // Failure after mutation: no false rollback promise.
}
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key) {
    (void)h; assert(!strcmp(key, "timezone"));
    if (!saved_size) return ESP_ERR_NVS_NOT_FOUND;
    saved_size = 0; return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }
static device_time_status status(void) { device_time_status s; device_time_get(&s); return s; }
int main(void) {
    const char *valid[] = {"UTC0", "CET-1CEST,M3.5.0/2,M10.5.0/3", "EST5EDT,M3.2.0/2,M11.1.0/2", "NST3:30NDT2:30,M3.2.0,M11.1.0", "ABC+5:45"};
    const char *invalid[] = {"", "Europe/Stockholm", "UTC", "UT0", "UTC24", "UTC0junk", "CET-1CEST", "CET-1CEST,M13.5.0,M10.5.0", "CET-1CEST,M3.0.0,M10.5.0", "CET-1CEST,M3.5.7,M10.5.0", "CET-1CEST,M3.5.0/24,M10.5.0", "UTC0 ", "UTC0;reboot", "ABC0DEF,M3.5.0", "ABC0DEF,M3.5.0/2:60,M10.5.0", "ABC0DEF,M3.5.0/2:30:60,M10.5.0"};
    for (size_t i=0;i<sizeof(valid)/sizeof(*valid);++i) assert(device_timezone_valid(valid[i]));
    for (size_t i=0;i<sizeof(invalid)/sizeof(*invalid);++i) assert(!device_timezone_valid(invalid[i]));
    assert(!device_timezone_valid(NULL));
    // Parser sees every truncated prefix, including delimiters at the last byte.
    char prefix[96];
    for (size_t n=0;n<strlen(valid[1]);++n) { memcpy(prefix,valid[1],n); prefix[n]=0; (void)device_timezone_valid(prefix); }
    device_time_init();
    unsigned seconds; int64_t sampled;
    assert(!device_time_command_clock(&seconds,&sampled));
    assert(!status().usable && !status().synchronized_this_boot && status().sync_age_ms == -1);
    assert(!strcmp(status().timezone,"UTC0") && status().config_error == ESP_OK);
    assert(device_time_manual(0) == ESP_ERR_INVALID_ARG);
    set_error = ESP_FAIL;
    assert(device_time_manual(1767225599) == ESP_FAIL && !status().usable);
    set_error = 0;
    assert(device_time_manual(1767225599) == ESP_OK); // 2025-12-31 23:59:59Z
    assert(device_time_command_clock(&seconds,&sampled) && seconds == 86399 && sampled == now_ms);
    now_ms += 2000;
    assert(status().local_seconds == 1 && status().source == TIME_MANUAL && !status().synchronized_this_boot);
    device_time_synchronized(1767225600); // backward correction
    assert(status().source == TIME_NETWORK && status().synchronized_this_boot && status().sync_age_ms == 0);
    device_time_network(false, ESP_OK); now_ms += 61000;
    assert(status().usable && status().sync_age_ms == 61000 && status().local_seconds == 61);
    device_time_network(true, ESP_OK); device_time_synchronized(1798761600); // forward correction
    assert(status().sync_age_ms == 0 && status().unix_seconds == 1798761600);
    assert(writes == 0);
    assert(device_time_timezone(valid[1]) == ESP_OK);
    assert(writes == 1 && status().local_seconds == 3600);
    // EU DST transitions 2026: 01:00 UTC on March 29 / October 25.
    device_time_synchronized(1774745999); assert(status().local_seconds == 7199); // 01:59:59 CET
    now_ms += 1000; assert(status().local_seconds == 10800); // 03:00:00 CEST
    device_time_synchronized(1792889999); assert(status().local_seconds == 10799); // 02:59:59 CEST
    now_ms += 1000; assert(status().local_seconds == 7200); // 02:00:00 CET
    assert(device_time_manual(1767225600) == ESP_OK);
    assert(status().source == TIME_MANUAL && status().synchronized_this_boot && status().sync_age_ms == 1000);
    device_time_synchronized(0); assert(status().source == TIME_MANUAL); // invalid sync ignored
    device_time_init(); // Configuration only survives, never clock validity.
    assert(!strcmp(status().timezone,valid[1]) && !status().usable && !status().synchronized_this_boot);
    storage_error = ESP_FAIL;
    assert(device_time_timezone("EST5") == ESP_FAIL);
    assert(status().persistence_uncertain && !strcmp(status().timezone,valid[1]));
    storage_error = 0; device_time_init(); // actual flash may expose failed write
    assert(!strcmp(status().timezone,"EST5") && !status().usable);
    saved[0] = 2; device_time_init();
    assert(status().config_error != ESP_OK && !strcmp(status().timezone,"UTC0"));
    saved_size = sizeof(saved); memset(saved, 'x', sizeof(saved)); device_time_init();
    assert(status().config_error != ESP_OK);
    assert(device_time_timezone("UTC0") == ESP_OK && status().config_error == ESP_OK && !status().persistence_uncertain);
    assert(device_time_manual(DEVICE_TIME_MAX) == ESP_OK); now_ms += 1000;
    assert(!status().usable && !device_time_command_clock(&seconds,&sampled));
    puts("Device time: provenance, offline/corrections, monotonic ages, DST, validation, persistence/restart and failed writes: passed");
}
