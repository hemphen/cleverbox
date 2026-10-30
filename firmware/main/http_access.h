// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include "esp_http_server.h"
typedef enum { HTTP_INTERFACE_UNKNOWN, HTTP_INTERFACE_AP, HTTP_INTERFACE_STA } http_interface;
http_interface http_request_interface(httpd_req_t *request);
const char *http_interface_name(http_interface interface);
