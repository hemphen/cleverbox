// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
#define DEVICE_TZ_BYTES 95
#define DEVICE_TZ_DEFAULT "UTC0"
#define DEVICE_TIME_MIN 1577836800LL /* 2020-01-01 UTC */
#define DEVICE_TIME_MAX 4102444799LL /* 2099-12-31 UTC */
typedef enum { TIME_UNSET, TIME_MANUAL, TIME_NETWORK } device_time_source;
typedef struct {
    bool usable; // SNTP synced or manual time set during this boot.
    device_time_source source;
    int64_t unix_seconds, sampled_ms;
    unsigned local_seconds;
    char timezone[DEVICE_TZ_BYTES + 1], local[32], utc[32];
} device_time_status;

// Platform start: loads the time zone and starts SNTP.
esp_err_t device_time_start(void);
// Portable core, shared by firmware and host simulation.
void device_time_init(void);
void device_time_get(device_time_status *out);
// Local seconds since midnight for command frames; false without usable time.
bool device_time_command_clock(unsigned *seconds, int64_t *sampled_ms);
bool device_timezone_valid(const char *tz);
esp_err_t device_time_timezone(const char *tz);
esp_err_t device_time_manual(int64_t unix_seconds);
void device_time_synchronized(int64_t unix_seconds);
#ifdef __cplusplus
}
#endif
