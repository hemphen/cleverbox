// SPDX-License-Identifier: MIT
#pragma once
#include "network_policy.h"
// Public, fixed setup credentials for every boot and recovery path.
#define NETWORK_AP_SSID "Cleverbox"
#define NETWORK_AP_PASSWORD "cleverbox"
#define NETWORK_BUTTON_GPIO 9
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t network_setup_start(void);
esp_err_t network_get_status(network_status *);
esp_err_t network_configure(const network_credentials *);
esp_err_t network_forget(void);
esp_err_t network_finish_recovery(void);
// Separate key in the existing namespace; never erases the NVS partition.
esp_err_t network_store_load(network_credentials *);
esp_err_t network_store_save(const network_credentials *);
esp_err_t network_store_forget(void);
#ifdef __cplusplus
}
#endif
