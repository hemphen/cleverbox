// SPDX-License-Identifier: MIT
#include <stdio.h>
#include "http_api.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "network_setup.h"
#include "device_time.h"
#include "mqtt_bridge.h"
#include "nvs_flash.h"
#include "mode_store.h"
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
    esp_err_t storage_error = nvs_flash_init();
    if (storage_error != ESP_OK)
        ESP_LOGE("storage", "NVS unavailable (%d); preserving flash, BOOT recovery remains available", storage_error);
    // Establish the immutable boot decision before starting any settings owner.
    // Failure keeps diagnostics/recovery available, but their writes must first
    // establish that same decision through the shared persistence gate.
    bool control_enabled;
    esp_err_t mode_error = device_mode_start(&control_enabled);
    if (mode_error != ESP_OK)
        ESP_LOGE("storage", "RF mode bootstrap failed (%d); RF disabled, settings persistence gated", mode_error);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(device_time_start());
    ESP_ERROR_CHECK(network_setup_start());
    radio_transport_start();
    ESP_ERROR_CHECK(mqtt_bridge_start());
    httpd_handle_t server;
    httpd_config_t http = HTTPD_DEFAULT_CONFIG();
    http.stack_size = 8192;
    http.max_uri_handlers = 16; // Root + control/setup + MQTT GET/POST.
    ESP_ERROR_CHECK(httpd_start(&server, &http));
    httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = home};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root));
    http_api_register(server);
}
