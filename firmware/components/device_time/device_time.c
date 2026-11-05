// SPDX-License-Identifier: MIT
#define _POSIX_C_SOURCE 200809L
#include "device_time.h"
#include "device_store.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Wall time is an anchor (UTC seconds at a monotonic instant) set by SNTP or a
// manual set during this boot. Nothing is trusted at boot.
static SemaphoreHandle_t mutex;
static device_time_source source;
static int64_t anchor_utc, anchor_ms;
static char timezone[DEVICE_TZ_BYTES + 1];

// Bounded POSIX subset that newlib and host libc handle alike:
// STDoffset[DST[offset],Mmonth.week.day[/time],Mmonth.week.day[/time]].
static bool number(const char **p, unsigned low, unsigned high)
{
    unsigned n = 0, count = 0;
    while (**p >= '0' && **p <= '9') {
        if (++count > 2) return false;
        n = n * 10 + (unsigned)(*(*p)++ - '0');
    }
    return count && n >= low && n <= high;
}
static bool name(const char **p)
{
    unsigned n = 0;
    while ((**p >= 'A' && **p <= 'Z') || (**p >= 'a' && **p <= 'z')) { ++*p; ++n; }
    return n >= 3 && n <= 10;
}
static bool offset(const char **p, bool sign)
{
    if (sign && (**p == '+' || **p == '-')) ++*p;
    if (!number(p, 0, 23)) return false;
    if (**p == ':') {
        ++*p; if (!number(p, 0, 59)) return false;
        if (**p == ':') { ++*p; if (!number(p, 0, 59)) return false; }
    }
    return true;
}
static bool rule(const char **p)
{
    if (*(*p)++ != 'M' || !number(p, 1, 12) || *(*p)++ != '.' ||
        !number(p, 1, 5) || *(*p)++ != '.' || !number(p, 0, 6)) return false;
    if (**p == '/') { ++*p; if (!offset(p, false)) return false; }
    return true;
}
bool device_timezone_valid(const char *tz)
{
    if (!tz || !*tz || strlen(tz) > DEVICE_TZ_BYTES) return false;
    const char *p = tz;
    if (!name(&p) || !offset(&p, true)) return false;
    if (!*p) return true;
    if (!name(&p)) return false;
    if (*p != ',' && !offset(&p, true)) return false;
    return *p++ == ',' && rule(&p) && *p++ == ',' && rule(&p) && !*p;
}

static void apply_timezone(const char *tz)
{
    strcpy(timezone, tz);
    setenv("TZ", tz, 1);
    tzset();
}

void device_time_init(void)
{
    if (!mutex) mutex = xSemaphoreCreateMutex();
    source = TIME_UNSET; anchor_utc = anchor_ms = 0;
    char tz[DEVICE_TZ_BYTES + 1] = {0};
    size_t size = sizeof(tz) - 1;
    bool loaded = store_load("timezone", tz, &size) == ESP_OK && device_timezone_valid(tz);
    apply_timezone(loaded ? tz : DEVICE_TZ_DEFAULT);
}

static void set_anchor(int64_t seconds, device_time_source from)
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    anchor_utc = seconds; anchor_ms = esp_timer_get_time() / 1000; source = from;
    xSemaphoreGive(mutex);
}

void device_time_synchronized(int64_t seconds)
{
    if (seconds >= DEVICE_TIME_MIN && seconds <= DEVICE_TIME_MAX) set_anchor(seconds, TIME_NETWORK);
}

esp_err_t device_time_manual(int64_t seconds)
{
    if (seconds < DEVICE_TIME_MIN || seconds > DEVICE_TIME_MAX) return ESP_ERR_INVALID_ARG;
    set_anchor(seconds, TIME_MANUAL);
    return ESP_OK;
}

void device_time_get(device_time_status *out)
{
    memset(out, 0, sizeof(*out));
    xSemaphoreTake(mutex, portMAX_DELAY);
    out->source = source;
    out->sampled_ms = esp_timer_get_time() / 1000;
    out->unix_seconds = anchor_utc + (out->sampled_ms - anchor_ms) / 1000;
    strcpy(out->timezone, timezone);
    out->usable = source != TIME_UNSET;
    if (out->usable) {
        time_t epoch = (time_t)out->unix_seconds;
        struct tm local, utc;
        localtime_r(&epoch, &local);
        gmtime_r(&epoch, &utc);
        out->local_seconds = (unsigned)(local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec);
        strftime(out->local, sizeof(out->local), "%Y-%m-%dT%H:%M:%S%z", &local);
        strftime(out->utc, sizeof(out->utc), "%Y-%m-%dT%H:%M:%SZ", &utc);
    }
    xSemaphoreGive(mutex);
}

bool device_time_command_clock(unsigned *seconds, int64_t *sampled_ms)
{
    device_time_status s;
    device_time_get(&s);
    *seconds = s.local_seconds; *sampled_ms = s.sampled_ms;
    return s.usable;
}

esp_err_t device_time_timezone(const char *tz)
{
    if (!device_timezone_valid(tz)) return ESP_ERR_INVALID_ARG;
    esp_err_t e = store_save("timezone", tz, strlen(tz) + 1);
    xSemaphoreTake(mutex, portMAX_DELAY);
    apply_timezone(tz);
    xSemaphoreGive(mutex);
    return e;
}
