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
    bool usable, synchronized_this_boot, network_connected, persistence_uncertain;
    device_time_source source;
    int64_t unix_seconds, sampled_ms, sync_age_ms;
    unsigned local_seconds;
    char timezone[DEVICE_TZ_BYTES + 1], local[32], utc[32];
    esp_err_t config_error, sntp_error;
} device_time_status;
esp_err_t device_time_start(void);
void device_time_get(device_time_status *out);
bool device_time_command_clock(unsigned *seconds, int64_t *sampled_ms);
bool device_timezone_valid(const char *tz);
esp_err_t device_time_timezone(const char *tz);
esp_err_t device_time_manual(int64_t unix_seconds);
// Adapter boundary: only explicit manual sets and successful SNTP notifications
// establish usable time. Boot never trusts RTC/host time or a persisted timestamp.
void device_time_init(void);
void device_time_synchronized(int64_t unix_seconds);
void device_time_network(bool connected, esp_err_t sntp_error);
void time_platform_lock(void);
void time_platform_unlock(void);
esp_err_t time_platform_set_utc(int64_t unix_seconds);
esp_err_t time_store_load(char out[DEVICE_TZ_BYTES + 1]);
esp_err_t time_store_save(const char *tz);
#ifdef __cplusplus
}
#endif
