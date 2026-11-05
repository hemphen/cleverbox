// SPDX-License-Identifier: MIT
// Minimal esp_http_server stand-in: serve.py passes each request to host_request().
#define _POSIX_C_SOURCE 200809L
#include "esp_http_server.h"
#include "http_api.h"
#include "device_time.h"
#include "mqtt_bridge.h"
#include "network_setup.h"
#include "simulation.h"
#include <stdlib.h>
#include <string.h>

static httpd_uri_t routes[16];
static size_t route_count;
static httpd_req_t request;

esp_err_t httpd_register_uri_handler(httpd_handle_t server, const httpd_uri_t *route)
{
    (void)server;
    if (route_count == sizeof(routes) / sizeof(routes[0])) return ESP_FAIL;
    routes[route_count++] = *route;
    return ESP_OK;
}
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *name, char *out, size_t n)
{
    if (strcmp(name, "Content-Type") || !r->content_type || strlen(r->content_type) >= n) return ESP_FAIL;
    strcpy(out, r->content_type);
    return ESP_OK;
}
int httpd_req_recv(httpd_req_t *r, char *out, size_t n)
{
    if (n > r->content_len - r->consumed) n = r->content_len - r->consumed;
    if (n > 100) n = 100; // Exercise partial reads.
    memcpy(out, r->body + r->consumed, n);
    r->consumed += n;
    return (int)n;
}
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *s) { r->status = s; return ESP_OK; }
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *s) { r->response_type = s; return ESP_OK; }
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *k, const char *v) { (void)r; (void)k; (void)v; return ESP_OK; }
esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *s)
{
    free(r->response);
    r->response = strdup(s);
    return ESP_OK;
}

void host_init(void)
{
    device_time_start();
    network_setup_start();
    simulation_init();
    mqtt_bridge_start();
    http_api_register(NULL);
}

int host_request(int method, const char *path, const char *body, const char *type)
{
    free(request.response);
    request = (httpd_req_t){.uri = path, .content_len = strlen(body), .body = body, .content_type = type,
                            .status = "404 Not Found", .response_type = "text/plain"};
    for (size_t i = 0; i < route_count; ++i)
        if (method == routes[i].method && !strcmp(path, routes[i].uri)) {
            if (routes[i].handler(&request) != ESP_OK && !request.response) {
                request.status = "500 Internal Server Error";
                httpd_resp_sendstr(&request, "Handler failed");
            }
            return atoi(request.status);
        }
    httpd_resp_sendstr(&request, "Not found");
    return 404;
}
const char *host_response(void) { return request.response ? request.response : ""; }
const char *host_response_type(void) { return request.response_type; }
