// SPDX-License-Identifier: MIT
// Production clock/store with the NVS cache/deferred-error model used for Wi-Fi.
#include "device_time.h"
#include "nvs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define LIMIT 128
static unsigned char durable[LIMIT], visible[LIMIT], deferred[LIMIT];
static size_t durable_size, visible_size, deferred_size;
static bool durable_present, visible_present, deferred_present, has_deferred;
static unsigned sets, erases, commits, opens, reads;
static unsigned fail_set, fail_erase, fail_commit, fail_open, fail_read;
static bool defer_error, mutate_error, mismatch_read;
int64_t esp_timer_get_time(void) { return 1000000; }
void time_platform_lock(void) {}
void time_platform_unlock(void) {}
esp_err_t time_platform_set_utc(int64_t seconds) { (void)seconds; return ESP_OK; }
static void copy(unsigned char *out, size_t *n, bool *present,
                 const unsigned char *in, size_t size, bool exists) {
    memcpy(out, in, size); *n = size; *present = exists;
}
static void reboot_nvs(void) {
    if (has_deferred) {
        copy(durable, &durable_size, &durable_present, deferred, deferred_size, deferred_present);
        has_deferred = false;
    }
    copy(visible, &visible_size, &visible_present, durable, durable_size, durable_present);
}
static void faults_clear(void) {
    sets = erases = commits = opens = reads = 0;
    fail_set = fail_erase = fail_commit = fail_open = fail_read = 0;
    defer_error = mutate_error = mismatch_read = false;
}
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *h) {
    assert(!strcmp(name, "cleverbox")); (void)mode;
    if (++opens == fail_open) return ESP_FAIL;
    *h = 1; return ESP_OK;
}
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *n) {
    (void)h; assert(!strcmp(key, "timezone"));
    if (++reads == fail_read) return ESP_FAIL;
    if (!visible_present) return ESP_ERR_NVS_NOT_FOUND;
    assert(out && *n >= visible_size);
    memcpy(out, visible, visible_size); *n = visible_size;
    if (mismatch_read && visible_size) ((unsigned char *)out)[0] ^= 1;
    return ESP_OK;
}
static void mutate(const void *in, size_t n, bool present) {
    assert(n <= LIMIT);
    copy(durable, &durable_size, &durable_present, in, n, present);
    copy(visible, &visible_size, &visible_present, in, n, present);
    has_deferred = false;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *in, size_t n) {
    (void)h; assert(!strcmp(key, "timezone"));
    // Matching cache reads/writes cannot prove that a deferred value is gone.
    if (visible_present && visible_size == n && !memcmp(visible, in, n)) return ESP_OK;
    if (++sets == fail_set) {
        if (defer_error) {
            copy(deferred, &deferred_size, &deferred_present, in, n, true);
            has_deferred = true;
        }
        if (mutate_error) mutate(in, n, true);
        return ESP_FAIL;
    }
    mutate(in, n, true); return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key) {
    (void)h; assert(!strcmp(key, "timezone"));
    if (++erases == fail_erase) return ESP_FAIL;
    if (!visible_present) return ESP_ERR_NVS_NOT_FOUND;
    mutate(visible, 0, false); return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ++commits == fail_commit ? ESP_FAIL : ESP_OK; }
static device_time_status status(void) { device_time_status s; device_time_get(&s); return s; }
static void seed(void) {
    faults_clear(); assert(device_time_timezone("UTC0") == ESP_OK);
    reboot_nvs(); device_time_init(); faults_clear();
    assert(!status().persistence_uncertain && !strcmp(status().timezone, "UTC0"));
}
static void expect_after_restart(const char *tz) {
    faults_clear(); reboot_nvs(); device_time_init();
    assert(status().config_error == ESP_OK && !strcmp(status().timezone, tz));
    assert(!status().usable && !status().synchronized_this_boot);
}
int main(void) {
    device_time_init();
    // Regression: UTC0 -> failed/deferred EST5 -> retry cached UTC0 -> restart.
    // Without erase/rewrite the retry succeeds against cache, then loads EST5.
    seed(); fail_set = 1; defer_error = true;
    assert(device_time_timezone("EST5") == ESP_FAIL);
    assert(status().persistence_uncertain && !strcmp(status().timezone, "UTC0") && has_deferred);
    faults_clear(); assert(device_time_timezone("UTC0") == ESP_OK);
    assert(!status().persistence_uncertain);
    expect_after_restart("UTC0");
    // Failed forced erase must not clear an existing uncertainty warning.
    seed(); fail_set = 1; defer_error = true;
    assert(device_time_timezone("EST5") == ESP_FAIL);
    faults_clear(); fail_erase = 1;
    assert(device_time_timezone("UTC0") == ESP_FAIL && status().persistence_uncertain && has_deferred);
    faults_clear(); assert(device_time_timezone("UTC0") == ESP_OK);
    expect_after_restart("UTC0");
    // When the failed value is already visible, retry requires a committed erase
    // followed by a committed rewrite. Every failure retains the warning/RAM rule.
    for (unsigned step = 0; step < 7; ++step) {
        seed(); fail_set = 1; mutate_error = true;
        assert(device_time_timezone("EST5") == ESP_FAIL && status().persistence_uncertain);
        faults_clear();
        switch (step) {
            case 0: fail_open = 1; break;
            case 1: fail_commit = 1; break; // Erase commit.
            case 2: fail_set = 1; break;
            case 3: fail_commit = 2; break; // Rewrite commit.
            case 4: fail_open = 2; break; // Fresh-handle verification.
            case 5: fail_read = 1; break;
            case 6: mismatch_read = true; break;
        }
        assert(device_time_timezone("UTC0") != ESP_OK);
        assert(status().persistence_uncertain && !strcmp(status().timezone, "UTC0"));
        faults_clear(); assert(device_time_timezone("UTC0") == ESP_OK);
        assert(!status().persistence_uncertain);
        expect_after_restart("UTC0");
    }
    // Also allow a successful retry of the failed candidate or another rule.
    seed(); fail_set = 1; defer_error = true;
    assert(device_time_timezone("EST5") == ESP_FAIL);
    faults_clear(); assert(device_time_timezone("EST5") == ESP_OK);
    expect_after_restart("EST5");
    seed(); fail_set = 1; defer_error = true;
    assert(device_time_timezone("EST5") == ESP_FAIL);
    faults_clear(); assert(device_time_timezone("CET-1") == ESP_OK);
    expect_after_restart("CET-1");
    puts("Timezone store: deferred write, forced retry/restart, failed erase/commits/verification and uncertainty retention: passed");
}
