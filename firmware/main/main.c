// SPDX-License-Identifier: MIT
#include <stdio.h>
#include "http_api.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "radio_transport.h"

extern const unsigned char page_start[] asm("_binary_index_html_start");
extern const unsigned char page_end[] asm("_binary_index_html_end");

static esp_err_t home(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_send(r, (const char *)page_start, page_end - page_start);
}

void app_main(void)
{
    // Never erase NVS automatically: it contains saved pairing data.
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    wifi_config_t config = {.ap = {.ssid = "Cleverbox", .channel = 1,
                                   .max_connection = 2, .authmode = WIFI_AUTH_WPA2_PSK}};
    // Bench access only. New password each boot, shown on the serial console.
    snprintf((char *)config.ap.password, sizeof(config.ap.password), "%08lx%08lx",
             (unsigned long)esp_random(), (unsigned long)esp_random());
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI("cleverbox", "Wi-Fi Cleverbox password: %s; open http://192.168.4.1",
             (char *)config.ap.password);
    radio_transport_start();
    httpd_handle_t server;
    httpd_config_t http = HTTPD_DEFAULT_CONFIG();
    http.stack_size = 8192;
    ESP_ERROR_CHECK(httpd_start(&server, &http));
    httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = home};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root));
    http_api_register(server);
}
