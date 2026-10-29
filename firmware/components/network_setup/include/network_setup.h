// SPDX-License-Identifier: MIT
#pragma once
#include "network_policy.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t network_setup_start(void);
esp_err_t network_get_status(network_status *);
esp_err_t network_configure(const network_credentials *);
esp_err_t network_forget(void);
// Separate key in the existing namespace; never erases the NVS partition.
esp_err_t network_store_load(network_credentials *);
esp_err_t network_store_save(const network_credentials *);
esp_err_t network_store_forget(void);
#ifdef __cplusplus
}
#endif
