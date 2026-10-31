// SPDX-License-Identifier: MIT
// Loopback destinations model simultaneous interfaces. No Wi-Fi claim is made.
#include "esp_netif.h"
#include <arpa/inet.h>
#include <string.h>
static esp_netif_t ap, sta;
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key) {
    ap.ip.ip.addr = inet_addr("127.0.0.2"); ap.ip.netmask.addr = inet_addr("255.255.255.255");
    sta.ip.ip.addr = inet_addr("127.0.0.1"); sta.ip.netmask.addr = inet_addr("255.255.255.255");
    return !strcmp(key, "WIFI_AP_DEF") ? &ap : !strcmp(key, "WIFI_STA_DEF") ? &sta : NULL;
}
bool esp_netif_is_netif_up(esp_netif_t *netif) { return netif != NULL; }
esp_err_t esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *ip) {
    *ip = netif->ip; return ESP_OK;
}
