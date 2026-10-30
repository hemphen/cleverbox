// SPDX-License-Identifier: MIT
#include "http_access.h"
#include "esp_netif.h"
#include <sys/socket.h>
#include <netinet/in.h>

static bool interface_ip(const char *key, esp_netif_ip_info_t *ip) {
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey(key);
    if (!netif) return false;
    if (!esp_netif_is_netif_up(netif)) return true; // Caller supplied zeroes.
    return esp_netif_get_ip_info(netif, ip) == ESP_OK;
}
http_interface http_request_interface(httpd_req_t *request) {
    // Use the accepted socket, never Host/Origin/forwarded headers or page state.
    struct sockaddr_storage local = {0}, peer = {0};
    socklen_t local_size = sizeof(local), peer_size = sizeof(peer);
    int fd = httpd_req_to_sockfd(request);
    if (fd < 0 || getsockname(fd, (struct sockaddr *)&local, &local_size) ||
        getpeername(fd, (struct sockaddr *)&peer, &peer_size) ||
        local.ss_family != AF_INET || peer.ss_family != AF_INET)
        return HTTP_INTERFACE_UNKNOWN;
    uint32_t destination = ((struct sockaddr_in *)&local)->sin_addr.s_addr;
    uint32_t source = ((struct sockaddr_in *)&peer)->sin_addr.s_addr;
    esp_netif_ip_info_t ap = {0}, sta = {0};
    if (!interface_ip("WIFI_AP_DEF", &ap) || !interface_ip("WIFI_STA_DEF", &sta))
        return HTTP_INTERFACE_UNKNOWN;
    bool has_ap = ap.ip.addr != 0, has_sta = sta.ip.addr != 0;
    if (has_ap && has_sta && ap.ip.addr == sta.ip.addr) return HTTP_INTERFACE_UNKNOWN;
    if (has_ap && destination == ap.ip.addr) return HTTP_INTERFACE_AP;
    if (!has_sta || destination != sta.ip.addr) return HTTP_INTERFACE_UNKNOWN;
    // Do not grant LAN control to an AP peer targeting the station address.
    // Overlapping AP/LAN subnets are deliberately conservative (mutations denied).
    if (has_ap && (!ap.netmask.addr ||
        (source & ap.netmask.addr) == (ap.ip.addr & ap.netmask.addr)))
        return HTTP_INTERFACE_UNKNOWN;
    return HTTP_INTERFACE_STA;
}
const char *http_interface_name(http_interface interface) {
    switch (interface) {
        case HTTP_INTERFACE_AP: return "ap";
        case HTTP_INTERFACE_STA: return "sta";
        default: return "unknown";
    }
}
