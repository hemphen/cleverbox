// SPDX-License-Identifier: MIT
#pragma once
#include <stddef.h>
#include <stdlib.h>
#include "esp_err.h"
typedef struct {
    size_t content_len, consumed;
    const char *body, *content_type;
    char *response;
    const char *status, *response_type;
} httpd_req_t;
typedef void *httpd_handle_t;
typedef struct { const char *uri; int method; esp_err_t (*handler)(httpd_req_t *); } httpd_uri_t;
#define HTTP_GET 0
#define HTTP_POST 1
#define HTTPD_400_BAD_REQUEST 400
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *, const char *, char *, size_t);
int httpd_req_recv(httpd_req_t *, char *, size_t);
esp_err_t httpd_resp_send_err(httpd_req_t *, int, const char *);
esp_err_t httpd_resp_set_status(httpd_req_t *, const char *);
esp_err_t httpd_resp_set_type(httpd_req_t *, const char *);
esp_err_t httpd_resp_set_hdr(httpd_req_t *, const char *, const char *);
esp_err_t httpd_resp_sendstr(httpd_req_t *, const char *);
esp_err_t httpd_register_uri_handler(httpd_handle_t, const httpd_uri_t *);
