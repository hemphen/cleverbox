// SPDX-License-Identifier: MIT
#pragma once
#include "esp_err.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
// Call once before starting settings owners. Repeated calls return the latched
// startup result. A failed initial write retains the original listen-only choice.
esp_err_t device_mode_start(bool *control_enabled);
// Must succeed before any application settings mutation. Retries only that
// initial choice. A known existing invalid record permits other-key repair,
// but RF stays closed and the mode itself requires an explicit mode save.
esp_err_t device_mode_prepare_write(void);
esp_err_t device_mode_load(bool *control_enabled, bool *stored);
esp_err_t device_mode_save(bool control_enabled);
#ifdef __cplusplus
}
#endif
