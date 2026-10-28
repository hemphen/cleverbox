// SPDX-License-Identifier: MIT
#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NOT_FOUND 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_INVALID_ARG 3
#define ESP_ERR_TIMEOUT 4
#define ESP_ERR_NO_MEM 5
#define ESP_FAIL 6
#define ESP_ERROR_CHECK(call) do { if ((call) != ESP_OK) abort(); } while (0)
