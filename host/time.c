// SPDX-License-Identifier: MIT
// No SNTP on the host; tests call host_time_sync() instead.
#include "device_time.h"
esp_err_t device_time_start(void) { device_time_init(); return ESP_OK; }
int host_time_sync(int64_t seconds)
{
    if (seconds < DEVICE_TIME_MIN || seconds > DEVICE_TIME_MAX) return 0;
    device_time_synchronized(seconds);
    return 1;
}
