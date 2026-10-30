// SPDX-License-Identifier: MIT
#include "http_access.h"
#include "esp_netif.h"
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static esp_netif_t ap, sta;
static bool ap_up = true, sta_up = true, lookup_fail, ip_fail, socket_fail;
static int family = AF_INET;
static uint32_t local, peer;
int httpd_req_to_sockfd(httpd_req_t *r) { return r->socket_fd; }
int getsockname(int fd, struct sockaddr *out, socklen_t *n) {
    (void)n; if (fd < 0 || socket_fail) return -1;
    struct sockaddr_in *v = (struct sockaddr_in *)out;
    v->sin_family = family; v->sin_addr.s_addr = local; return 0;
}
int getpeername(int fd, struct sockaddr *out, socklen_t *n) {
    int result = getsockname(fd, out, n);
    ((struct sockaddr_in *)out)->sin_addr.s_addr = peer; return result;
}
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key) {
    if (lookup_fail) return NULL;
    return !strcmp(key, "WIFI_AP_DEF") ? &ap : &sta;
}
bool esp_netif_is_netif_up(esp_netif_t *n) { return n == &ap ? ap_up : sta_up; }
esp_err_t esp_netif_get_ip_info(esp_netif_t *n, esp_netif_ip_info_t *ip) {
    *ip = n->ip; return ip_fail ? ESP_FAIL : ESP_OK;
}
int main(void) {
    httpd_req_t r = {.socket_fd = 1};
    ap.ip.ip.addr = inet_addr("192.168.4.1"); ap.ip.netmask.addr = inet_addr("255.255.255.0");
    sta.ip.ip.addr = inet_addr("192.0.2.10"); peer = inet_addr("192.0.2.20");
    local = sta.ip.ip.addr; assert(http_request_interface(&r) == HTTP_INTERFACE_STA);
    local = ap.ip.ip.addr; assert(http_request_interface(&r) == HTTP_INTERFACE_AP);
    peer = inet_addr("192.168.4.2"); local = sta.ip.ip.addr;
    assert(http_request_interface(&r) == HTTP_INTERFACE_UNKNOWN);
    ap.ip.ip.addr = sta.ip.ip.addr; assert(http_request_interface(&r) == HTTP_INTERFACE_UNKNOWN);
    ap.ip.ip.addr = inet_addr("192.168.4.1"); peer = inet_addr("192.0.2.20");
    local = 0; assert(http_request_interface(&r) == HTTP_INTERFACE_UNKNOWN);
    local = sta.ip.ip.addr; sta_up = false; assert(http_request_interface(&r) == HTTP_INTERFACE_UNKNOWN); sta_up = true;
    lookup_fail = true; assert(http_request_interface(&r) == HTTP_INTERFACE_UNKNOWN); lookup_fail = false;
    ip_fail = true; assert(http_request_interface(&r) == HTTP_INTERFACE_UNKNOWN); ip_fail = false;
    socket_fail = true; assert(http_request_interface(&r) == HTTP_INTERFACE_UNKNOWN); socket_fail = false;
    family = AF_INET6; assert(http_request_interface(&r) == HTTP_INTERFACE_UNKNOWN); family = AF_INET;
    r.socket_fd = -1; assert(http_request_interface(&r) == HTTP_INTERFACE_UNKNOWN);
    puts("HTTP socket classification: missing/failed/down/ambiguous/IPv6 and AP peer guard: passed");
}
