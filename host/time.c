// SPDX-License-Identifier: MIT
// UTC/locking boundary only. Never set the host's system clock or auto-sync it.
#include "device_time.h"
void time_platform_lock(void) {}
void time_platform_unlock(void) {}
esp_err_t time_platform_set_utc(int64_t seconds) { (void)seconds; return ESP_OK; }
esp_err_t device_time_start(void) { device_time_init(); return ESP_OK; }
int host_time_sync(int64_t seconds) {
    if (seconds < DEVICE_TIME_MIN || seconds > DEVICE_TIME_MAX) return 0;
    device_time_synchronized(seconds); return 1;
}
