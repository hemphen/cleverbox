// SPDX-License-Identifier: MIT
// Uses the host's file-backed NVS (host/storage.c).
#define _POSIX_C_SOURCE 200809L
#include "device_time.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int64_t now_ms = 1000;
int64_t esp_timer_get_time(void) { return now_ms * 1000; }
static device_time_status status(void) { device_time_status s; device_time_get(&s); return s; }

int main(void)
{
    char path[] = "/tmp/cleverbox-time-XXXXXX";
    int fd = mkstemp(path); assert(fd >= 0); close(fd); unlink(path);
    setenv("CLEVERBOX_SIM_STORAGE", path, 1);

    const char *valid[] = {"UTC0", "CET-1CEST,M3.5.0/2,M10.5.0/3", "EST5EDT,M3.2.0/2,M11.1.0/2", "NST3:30NDT2:30,M3.2.0,M11.1.0", "ABC+5:45"};
    const char *invalid[] = {"", "Europe/Stockholm", "UTC", "UT0", "UTC24", "UTC0junk", "CET-1CEST", "CET-1CEST,M13.5.0,M10.5.0",
                             "CET-1CEST,M3.0.0,M10.5.0", "CET-1CEST,M3.5.7,M10.5.0", "CET-1CEST,M3.5.0/24,M10.5.0", "UTC0 ",
                             "ABC0DEF,M3.5.0", "ABC0DEF,M3.5.0/2:60,M10.5.0"};
    for (size_t i = 0; i < sizeof(valid) / sizeof(*valid); ++i) assert(device_timezone_valid(valid[i]));
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) assert(!device_timezone_valid(invalid[i]));
    assert(!device_timezone_valid(NULL));

    device_time_init();
    unsigned seconds; int64_t sampled;
    assert(!device_time_command_clock(&seconds, &sampled) && !status().usable);
    assert(!strcmp(status().timezone, "UTC0"));
    assert(device_time_manual(0) == ESP_ERR_INVALID_ARG && !status().usable);
    assert(device_time_manual(1767225599) == ESP_OK); // 2025-12-31 23:59:59Z
    assert(device_time_command_clock(&seconds, &sampled) && seconds == 86399 && sampled == now_ms);
    now_ms += 2000;
    assert(status().local_seconds == 1 && status().source == TIME_MANUAL);
    device_time_synchronized(1798761600);
    assert(status().source == TIME_NETWORK && status().unix_seconds == 1798761600);
    device_time_synchronized(0); // Implausible server time is ignored.
    assert(status().unix_seconds == 1798761600);

    // Time zone persists; EU DST transitions in 2026 are at 01:00 UTC.
    assert(device_time_timezone("Bad zone") == ESP_ERR_INVALID_ARG);
    assert(device_time_timezone(valid[1]) == ESP_OK);
    device_time_synchronized(1774745999); assert(status().local_seconds == 7199);  // 01:59:59 CET
    now_ms += 1000;                       assert(status().local_seconds == 10800); // 03:00:00 CEST
    device_time_synchronized(1792889999); assert(status().local_seconds == 10799); // 02:59:59 CEST
    now_ms += 1000;                       assert(status().local_seconds == 7200);  // 02:00:00 CET

    // Restart: the time zone survives, the clock does not.
    device_time_init();
    assert(!strcmp(status().timezone, valid[1]) && !status().usable);
    char file[sizeof(path) + 16];
    snprintf(file, sizeof(file), "%s.timezone", path); unlink(file);
    puts("Device time: validation, manual/SNTP time, DST, time zone persistence and restart: passed");
}
