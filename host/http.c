// SPDX-License-Identifier: MIT
#define _POSIX_C_SOURCE 200809L
#include "esp_http_server.h"
#include "http_api.h"
#include "simulation.h"
#include "network_setup.h"
#include "device_time.h"
#include "mqtt_bridge.h"
#include "mode_store.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
static httpd_uri_t routes[15];
static size_t route_count;
static httpd_req_t request;
static int socket_fd = -1;
static cJSON *request_headers;
void host_http_headers(const char *json) { cJSON_Delete(request_headers); request_headers = cJSON_Parse(json); }
void host_http_socket(int fd) { socket_fd = fd; }
int httpd_req_to_sockfd(httpd_req_t *r) { return r->socket_fd; }
esp_err_t httpd_register_uri_handler(httpd_handle_t server, const httpd_uri_t *route) {
    (void)server;
    if (route_count == 15) return ESP_FAIL;
    routes[route_count++] = *route; return ESP_OK;
}
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *name, char *out, size_t n) {
    const char *value = !strcmp(name,"Content-Type") ? r->content_type :
        cJSON_GetStringValue(cJSON_GetObjectItem(request_headers, name));
    if (!value || strlen(value) >= n) return ESP_FAIL;
    strcpy(out,value); return ESP_OK;
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
void host_init(void) { bool enabled; device_mode_start(&enabled); simulation_init(); device_time_start(); network_setup_start(); mqtt_bridge_start(); http_api_register(NULL); }
int host_request(int method, const char *path, const char *body, const char *type) {
    free(request.response);
    request=(httpd_req_t){.uri=path,.content_len=strlen(body),.body=body,.content_type=type,
                         .status="200 OK",.response_type="text/plain",.socket_fd=socket_fd};
    for (size_t i=0;i<route_count;++i) if (method==routes[i].method && strlen(routes[i].uri)==strcspn(path,"?") && !strncmp(path,routes[i].uri,strcspn(path,"?"))) {
        request.user_ctx = routes[i].user_ctx;
        // A completed response plus ESP_FAIL intentionally closes an IDF session.
        if (routes[i].handler(&request)!=ESP_OK && !request.response) {
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

const char *host_simulation_snapshot(void) {
    static char *json;
    cJSON *status = cJSON_CreateObject(); simulation_status(status);
    free(json); json = cJSON_PrintUnformatted(status); cJSON_Delete(status);
    return json ? json : "{}";
}
