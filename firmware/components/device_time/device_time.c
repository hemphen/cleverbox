// SPDX-License-Identifier: MIT
#define _POSIX_C_SOURCE 200809L
#include "device_time.h"
#include "esp_timer.h"
#include "nvs.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

static device_time_status state;
static int64_t anchor_utc, anchor_ms, sync_ms;

// Deliberately bounded POSIX subset shared by newlib and host libc:
// STDoffset[DST[offset],Mmonth.week.day[/time],Mmonth.week.day[/time]].
// Explicit transition rules avoid libc-specific default DST rules.
static bool number(const char **p, unsigned low, unsigned high) {
    unsigned n = 0, count = 0;
    while (**p >= '0' && **p <= '9') {
        if (++count > 2) return false;
        n = n * 10 + (unsigned)(*(*p)++ - '0');
    }
    return count && n >= low && n <= high;
}
static bool name(const char **p) {
    unsigned n = 0;
    while ((**p >= 'A' && **p <= 'Z') || (**p >= 'a' && **p <= 'z')) { ++*p; ++n; }
    return n >= 3 && n <= 10;
}
static bool offset(const char **p, bool sign) {
    if (sign && (**p == '+' || **p == '-')) ++*p;
    if (!number(p, 0, 23)) return false;
    if (**p == ':') {
        ++*p; if (!number(p, 0, 59)) return false;
        if (**p == ':') { ++*p; if (!number(p, 0, 59)) return false; }
    }
    return true;
}
static bool rule(const char **p) {
    if (*(*p)++ != 'M' || !number(p, 1, 12) || *(*p)++ != '.' ||
        !number(p, 1, 5) || *(*p)++ != '.' || !number(p, 0, 6)) return false;
    if (**p == '/') { ++*p; if (!offset(p, false)) return false; }
    return true;
}
bool device_timezone_valid(const char *tz) {
    if (!tz || !*tz || strlen(tz) > DEVICE_TZ_BYTES) return false;
    const char *p = tz;
    if (!name(&p) || !offset(&p, true)) return false;
    if (!*p) return true;
    if (!name(&p)) return false;
    if (*p != ',' && !offset(&p, true)) return false;
    return *p++ == ',' && rule(&p) && *p++ == ',' && rule(&p) && !*p;
}
static bool valid_epoch(int64_t seconds) { return seconds >= DEVICE_TIME_MIN && seconds <= DEVICE_TIME_MAX; }
static esp_err_t apply_timezone(const char *tz) {
    if (setenv("TZ", tz, 1)) return ESP_ERR_NO_MEM;
    tzset(); strcpy(state.timezone, tz); return ESP_OK;
}
void device_time_init(void) {
    state = (device_time_status){.sync_age_ms = -1};
    anchor_utc = anchor_ms = sync_ms = 0;
    char tz[DEVICE_TZ_BYTES + 1];
    state.config_error = time_store_load(tz);
    if (state.config_error == ESP_ERR_NVS_NOT_FOUND) state.config_error = ESP_OK;
    else if (state.config_error == ESP_OK) {
        state.config_error = apply_timezone(tz);
        if (state.config_error == ESP_OK) return;
    }
    esp_err_t err = apply_timezone(DEVICE_TZ_DEFAULT);
    if (err != ESP_OK) state.config_error = err;
}
static void anchor(int64_t seconds, device_time_source source) {
    anchor_utc = seconds; anchor_ms = esp_timer_get_time() / 1000;
    state.source = source;
}
void device_time_synchronized(int64_t seconds) {
    if (!valid_epoch(seconds)) return; // Do not accept an implausible server epoch.
    time_platform_lock();
    anchor(seconds, TIME_NETWORK); sync_ms = anchor_ms;
    state.synchronized_this_boot = true;
    time_platform_unlock();
}
void device_time_network(bool connected, esp_err_t error) {
    time_platform_lock(); state.network_connected = connected; state.sntp_error = error; time_platform_unlock();
}
static void snapshot(device_time_status *out) {
    *out = state; out->sampled_ms = esp_timer_get_time() / 1000;
    out->sync_age_ms = state.synchronized_this_boot ? out->sampled_ms - sync_ms : -1;
    out->unix_seconds = anchor_utc + (out->sampled_ms - anchor_ms) / 1000;
    out->usable = state.source != TIME_UNSET && valid_epoch(out->unix_seconds) && *state.timezone;
    if (!out->usable) { out->unix_seconds = 0; return; }
    time_t epoch = (time_t)out->unix_seconds; struct tm local, utc;
    if (!localtime_r(&epoch, &local) || !gmtime_r(&epoch, &utc)) { out->usable = false; return; }
    out->local_seconds = (unsigned)(local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec);
    strftime(out->local, sizeof(out->local), "%Y-%m-%dT%H:%M:%S%z", &local);
    strftime(out->utc, sizeof(out->utc), "%Y-%m-%dT%H:%M:%SZ", &utc);
}
void device_time_get(device_time_status *out) {
    time_platform_lock(); snapshot(out); time_platform_unlock();
}
bool device_time_command_clock(unsigned *seconds, int64_t *sampled_ms) {
    device_time_status s; device_time_get(&s);
    if (!s.usable) return false;
    *seconds = s.local_seconds; *sampled_ms = s.sampled_ms; return true;
}
esp_err_t device_time_manual(int64_t seconds) {
    if (!valid_epoch(seconds)) return ESP_ERR_INVALID_ARG;
    time_platform_lock();
    esp_err_t err = time_platform_set_utc(seconds);
    if (err == ESP_OK) anchor(seconds, TIME_MANUAL);
    time_platform_unlock(); return err;
}
esp_err_t device_time_timezone(const char *tz) {
    if (!device_timezone_valid(tz)) return ESP_ERR_INVALID_ARG;
    // Configuration actions have one owner (the HTTP server task). Keep flash
    // I/O outside the clock lock so SNTP notifications never wait for NVS.
    // Only successful explicit writes change active configuration. An NVS error
    // may still have changed flash: report uncertainty, never promise rollback.
    esp_err_t err = time_store_save(tz);
    time_platform_lock();
    state.persistence_uncertain = err != ESP_OK;
    if (err == ESP_OK) err = apply_timezone(tz);
    state.config_error = err;
    time_platform_unlock(); return err;
}
