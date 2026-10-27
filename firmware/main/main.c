// SPDX-License-Identifier: MIT
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "radiator_protocol.h"
#include "radio_transport.h"

extern const unsigned char page_start[] asm("_binary_index_html_start");
extern const unsigned char page_end[] asm("_binary_index_html_end");

static esp_err_t home(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_send(r, (const char *)page_start, page_end - page_start);
}

static const char *string(cJSON *j, const char *key)
{
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, key));
}

static bool address(cJSON *j, const char *key, uint8_t out[4])
{
    size_t n = 0;
    return rf_parse_hex(string(j, key), out, 4, &n) && n == 4;
}

static esp_err_t preview(httpd_req_t *r)
{
    char body[1024], type[40];
    if (httpd_req_get_hdr_value_str(r, "Content-Type", type, sizeof(type)) != ESP_OK ||
        strcmp(type, "application/json") != 0)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Use application/json");
    if (r->content_len == 0 || r->content_len >= sizeof(body))
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Invalid body length");
    size_t used = 0;
    while (used < r->content_len) {
        int n = httpd_req_recv(r, body + used, r->content_len - used);
        if (n <= 0) return ESP_FAIL;
        used += (size_t)n;
    }
    body[used] = 0;
    cJSON *j = cJSON_ParseWithOpts(body, NULL, true);
    rf_frame f = {0}; bool ok = false;
    const char *op = string(j, "command");
    if (op && strcmp(op, "target") == 0) {
        rf_frame state = {0};
        cJSON *mode = cJSON_GetObjectItemCaseSensitive(j, "mode");
        cJSON *temp = cJSON_GetObjectItemCaseSensitive(j, "centi_c");
        if (cJSON_IsNumber(mode) && mode->valuedouble >= 0 && mode->valuedouble <= 3 &&
            mode->valuedouble == mode->valueint && cJSON_IsNumber(temp) &&
            isfinite(temp->valuedouble) && temp->valuedouble >= 0 && temp->valuedouble <= 3000 &&
            temp->valuedouble == temp->valueint &&
            rf_parse_hex(string(j, "state"), state.bytes, sizeof(state.bytes), &state.size))
            ok = rf_set_target(&f, &state, (rf_mode)mode->valueint, temp->valueint);
    } else if (op) {
        uint8_t src[4], dst[4];
        rf_command command;
        bool known = true;
        if (strcmp(op, "offer") == 0) command = RF_OFFER;
        else if (strcmp(op, "bind") == 0) command = RF_BIND;
        else if (strcmp(op, "poll") == 0) command = RF_POLL;
        else { command = RF_POLL; known = false; }
        if (known && address(j, "source", src) && address(j, "destination", dst))
            ok = rf_short(&f, src, dst, command);
    }
    cJSON_Delete(j);
    if (!ok) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Invalid command or frame");
    char hex[RF_MAX_FRAME * 2 + 1], response[640];
    rf_to_hex(&f, hex);
    snprintf(response, sizeof(response),
             "{\"status\":\"preview\",\"transmitted\":false,\"frame\":\"%s\"}", hex);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_sendstr(r, response);
}

static esp_err_t radio_status_page(httpd_req_t *r)
{
    radio_status s = radio_transport_status();
    char body[128];
    snprintf(body, sizeof(body), "{\"ready\":%s,\"error\":%d,\"chip_version\":%d}",
             s.ready ? "true" : "false", s.error, s.chip_version);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_sendstr(r, body);
}

void app_main(void)
{
    // Do not erase NVS automatically: later versions store pairing here.
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
    httpd_uri_t command = {.uri = "/api/preview", .method = HTTP_POST, .handler = preview};
    httpd_uri_t radio_info = {.uri = "/api/radio", .method = HTTP_GET, .handler = radio_status_page};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &radio_info));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &command));
}
