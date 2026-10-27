// SPDX-License-Identifier: MIT
#pragma once
#include "esp_err.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { uint8_t controller[4], radiator[4]; } paired_device;
// ESP_ERR_NOT_FOUND means no saved pairing. Other errors must be shown.
esp_err_t device_store_load(paired_device *device);
esp_err_t device_store_save(const paired_device *device);
#ifdef __cplusplus
}
#endif
