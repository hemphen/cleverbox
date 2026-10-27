// SPDX-License-Identifier: MIT
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
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

static esp_err_t pairing_action(httpd_req_t *r)
{
    char body[256], type[40];
    if (httpd_req_get_hdr_value_str(r, "Content-Type", type, sizeof(type)) != ESP_OK ||
        strcmp(type, "application/json") || !r->content_len || r->content_len >= sizeof(body))
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Use a small JSON request");
    size_t used = 0;
    while (used < r->content_len) {
        int n = httpd_req_recv(r, body + used, r->content_len - used);
        if (n <= 0) return ESP_FAIL;
        used += (size_t)n;
    }
    body[used] = 0;
    cJSON *j = cJSON_ParseWithOpts(body, NULL, true);
    const char *action = string(j, "action");
    esp_err_t result = ESP_ERR_INVALID_ARG;
    if (action && !strcmp(action, "start")) {
        uint8_t source[4], destination[4];
        rf_pairing check = {0};
        if (address(j, "source", source) && address(j, "destination", destination) &&
            rf_pair_start(&check, source, destination))
            result = radio_pair_start(source, destination);
    } else if (action && !strcmp(action, "cancel")) result = radio_pair_cancel();
    else if (action && !strcmp(action, "save")) result = radio_pair_save();
    cJSON_Delete(j);
    if (result == ESP_ERR_INVALID_ARG)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Invalid pairing action or addresses");
    if (result != ESP_OK) {
        httpd_resp_set_status(r, "409 Conflict");
        return httpd_resp_sendstr(r, "Radio unavailable, pairing busy, already saved, or storage blocked. Check radio status.");
    }
    httpd_resp_set_status(r, "202 Accepted");
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, "{\"accepted\":true}");
}

static bool integer(cJSON *j, const char *key, int low, int high, int *out)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(j, key);
    if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) ||
        value->valuedouble < low || value->valuedouble > high ||
        value->valuedouble != value->valueint) return false;
    *out = value->valueint;
    return true;
}

static esp_err_t live_command(httpd_req_t *r)
{
    char body[512], type[40];
    if (httpd_req_get_hdr_value_str(r, "Content-Type", type, sizeof(type)) != ESP_OK ||
        strcmp(type, "application/json") || !r->content_len || r->content_len >= sizeof(body))
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Use a small JSON request");
    size_t used = 0;
    while (used < r->content_len) {
        int n = httpd_req_recv(r, body + used, r->content_len - used);
        if (n <= 0) return ESP_FAIL;
        used += (size_t)n;
    }
    body[used] = 0;
    cJSON *j = cJSON_ParseWithOpts(body, NULL, true);
    rf_control_request command = {0};
    int mode, target, clock;
    bool valid = integer(j, "mode", 0, 3, &mode) && integer(j, "centi_c", 0, 3000, &target) &&
                 integer(j, "clock_seconds", 0, 86399, &clock) &&
                 rf_parse_hex(string(j, "state"), command.state.bytes,
                              sizeof(command.state.bytes), &command.state.size);
    if (valid) {
        command.mode = (rf_mode)mode; command.centi_c = target; command.clock_seconds = clock;
    }
    cJSON_Delete(j);
    esp_err_t result = valid ? radio_control_start(&command) : ESP_ERR_INVALID_ARG;
    if (result == ESP_ERR_INVALID_ARG)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Invalid target, mode, clock or radiator template");
    if (result != ESP_OK) {
        httpd_resp_set_status(r, "409 Conflict");
        return httpd_resp_sendstr(r, "Command busy, radio unavailable or no saved pairing");
    }
    httpd_resp_set_status(r, "202 Accepted");
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, "{\"status\":\"queued\"}");
}

static void address_text(const uint8_t a[4], char out[9])
{
    snprintf(out, 9, "%02X%02X%02X%02X", a[0], a[1], a[2], a[3]);
}

static esp_err_t radio_status_page(httpd_req_t *r)
{
    radio_status s = radio_transport_status();
    int64_t now = esp_timer_get_time();
    cJSON *j = cJSON_CreateObject();
    if (!j) return ESP_ERR_NO_MEM;
    cJSON_AddBoolToObject(j, "ready", s.ready);
    cJSON_AddNumberToObject(j, "error", s.error);
    cJSON_AddNumberToObject(j, "chip_version", s.chip_version);
    cJSON_AddNumberToObject(j, "valid_frames", s.valid_frames);
    cJSON_AddNumberToObject(j, "invalid_frames", s.invalid_frames);
    cJSON_AddNumberToObject(j, "receive_errors", s.receive_errors);
    cJSON *pair = cJSON_AddObjectToObject(j, "pairing");
    cJSON_AddStringToObject(pair, "phase", rf_pair_phase_name(s.pairing.phase));
    cJSON_AddBoolToObject(pair, "saved", s.pairing_saved);
    cJSON_AddNumberToObject(pair, "error", s.pairing_error);
    char controller[9], radiator[9];
    address_text(s.pairing.controller, controller);
    address_text(s.pairing.radiator, radiator);
    cJSON_AddStringToObject(pair, "controller", controller);
    cJSON_AddStringToObject(pair, "radiator", radiator);
    cJSON *command = cJSON_AddObjectToObject(j, "command");
    cJSON_AddStringToObject(command, "phase", rf_control_phase_name(s.control.phase));
    cJSON_AddNumberToObject(command, "error", s.control_error);
    cJSON_AddNumberToObject(command, "attempts", s.control.attempts);
    if (s.control.phase != RF_CONTROL_IDLE) {
        cJSON_AddNumberToObject(command, "requested_target_c", s.control.centi_c / 100.0);
        cJSON_AddNumberToObject(command, "requested_mode", s.control.mode);
    }
    if (s.control.phase == RF_CONTROL_MATCHED)
        cJSON_AddNumberToObject(command, "match_age_ms", now / 1000 - s.control.matched_ms);
    if (s.has_paired_report) {
        cJSON *observed = cJSON_AddObjectToObject(j, "observed");
        int64_t age = (now - s.paired_report_time_us) / 1000;
        cJSON_AddNumberToObject(observed, "age_ms", age);
        cJSON_AddBoolToObject(observed, "stale", age >= RF_REPORT_STALE_MS);
        cJSON_AddNumberToObject(observed, "mode", s.paired_report.mode);
        if (s.paired_report.temperature_available)
            cJSON_AddNumberToObject(observed, "temperature_c", s.paired_report.temperature_centi_c / 100.0);
        else cJSON_AddNullToObject(observed, "temperature_c");
        if (s.paired_report.target_available)
            cJSON_AddNumberToObject(observed, "target_c", s.paired_report.target_centi_c / 100.0);
        else cJSON_AddNullToObject(observed, "target_c");
    }
    if (s.has_frame) {
        char hex[RF_MAX_FRAME * 2 + 1];
        rf_to_hex(&s.last_frame, hex);
        cJSON_AddStringToObject(j, "last_frame", hex);
        cJSON_AddNumberToObject(j, "frame_age_ms", (now - s.frame_time_us) / 1000);
    }
    if (s.has_report) {
        cJSON *report = cJSON_AddObjectToObject(j, "report");
        char source[9], destination[9];
        address_text(s.last_report.source, source);
        address_text(s.last_report.destination, destination);
        cJSON_AddStringToObject(report, "source", source);
        cJSON_AddStringToObject(report, "destination", destination);
        cJSON_AddNumberToObject(report, "age_ms", (now - s.report_time_us) / 1000);
        if (s.last_report.temperature_available)
            cJSON_AddNumberToObject(report, "temperature_c", s.last_report.temperature_centi_c / 100.0);
        else cJSON_AddNullToObject(report, "temperature_c");
        if (s.last_report.target_available)
            cJSON_AddNumberToObject(report, "target_c", s.last_report.target_centi_c / 100.0);
        else cJSON_AddNullToObject(report, "target_c");
        cJSON_AddNumberToObject(report, "mode", s.last_report.mode);
    }
    char *body = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!body) return ESP_ERR_NO_MEM;
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    esp_err_t result = httpd_resp_sendstr(r, body);
    cJSON_free(body);
    return result;
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
    httpd_uri_t command = {.uri = "/api/preview", .method = HTTP_POST, .handler = preview};
    httpd_uri_t radio_info = {.uri = "/api/radio", .method = HTTP_GET, .handler = radio_status_page};
    httpd_uri_t pair = {.uri = "/api/pairing", .method = HTTP_POST, .handler = pairing_action};
    httpd_uri_t live = {.uri = "/api/command", .method = HTTP_POST, .handler = live_command};
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &live));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &pair));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &radio_info));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &command));
}
