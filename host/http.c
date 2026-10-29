// SPDX-License-Identifier: MIT
#define _POSIX_C_SOURCE 200809L
#include "esp_http_server.h"
#include "http_api.h"
#include "simulation.h"
#include "network_setup.h"
#include "device_time.h"
#include "mqtt_bridge.h"
#include <stdio.h>
#include <string.h>
static httpd_uri_t routes[13];
static size_t route_count;
static httpd_req_t request;
esp_err_t httpd_register_uri_handler(httpd_handle_t server, const httpd_uri_t *route) {
    (void)server;
    if (route_count == 13) return ESP_FAIL;
    routes[route_count++] = *route; return ESP_OK;
}
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *name, char *out, size_t n) {
    if (strcmp(name,"Content-Type") || strlen(r->content_type) >= n) return ESP_FAIL;
    strcpy(out,r->content_type); return ESP_OK;
}
int httpd_req_recv(httpd_req_t *r, char *out, size_t n) {
    if (n > r->content_len - r->consumed) n = r->content_len - r->consumed;
    // Exercise the handlers' partial-read loops too.
    if (n > 17) n = 17;
    memcpy(out,r->body + r->consumed,n); r->consumed += n; return (int)n;
}
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *s) { r->status=s; return ESP_OK; }
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *s) { r->response_type=s; return ESP_OK; }
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *k, const char *v) { (void)r; (void)k; (void)v; return ESP_OK; }
esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *s) {
    free(r->response); r->response=strdup(s); return r->response ? ESP_OK : ESP_ERR_NO_MEM;
}
esp_err_t httpd_resp_send_err(httpd_req_t *r, int code, const char *s) {
    (void)code; r->status="400 Bad Request"; return httpd_resp_sendstr(r,s);
}
void host_init(void) { simulation_init(); device_time_start(); network_setup_start(); mqtt_bridge_start(); http_api_register(NULL); }
int host_request(int method, const char *path, const char *body, const char *type) {
    free(request.response);
    request=(httpd_req_t){.uri=path,.content_len=strlen(body),.body=body,.content_type=type,
                         .status="200 OK",.response_type="text/plain"};
    for (size_t i=0;i<route_count;++i) if (method==routes[i].method && strlen(routes[i].uri)==strcspn(path,"?") && !strncmp(path,routes[i].uri,strcspn(path,"?"))) {
        if (routes[i].handler(&request)!=ESP_OK) {
            request.status="500 Internal Server Error"; httpd_resp_sendstr(&request,"Handler failed");
        }
        return atoi(request.status);
    }
    request.status="404 Not Found"; httpd_resp_sendstr(&request,"Not found"); return 404;
}
const char *host_response(void) { return request.response ? request.response : ""; }
const char *host_response_type(void) { return request.response_type; }

size_t httpd_req_get_url_query_len(httpd_req_t *r) { const char *q = strchr(r->uri, '?'); return q ? strlen(q + 1) : 0; }
esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *out, size_t n) {
    const char *q = strchr(r->uri, '?'); if (!q || strlen(q + 1) >= n) return ESP_FAIL;
    strcpy(out, q + 1); return ESP_OK;
}
