// SPDX-License-Identifier: MIT
// Isolated store tests fake the boot prerequisite, alongside their fake NVS.
// Never linked into firmware or the production host simulator.
#include "mode_store.h"
esp_err_t device_mode_prepare_write(void) { return ESP_OK; }
