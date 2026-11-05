// SPDX-License-Identifier: MIT
// Local JSON API. See docs/http-api.md.
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "http_api.h"
#include "device_time.h"
#include "mqtt_bridge.h"
#include "network_setup.h"
#include "radio_transport.h"
#ifdef CLEVERBOX_HOST
#include "simulation.h"
#endif

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

// Parse a JSON request body (application/json, under 1 KiB). NULL if invalid.
static cJSON *read_json(httpd_req_t *r)
{
    char body[1024];
    if (httpd_req_get_hdr_value_str(r, "Content-Type", body, sizeof(body)) != ESP_OK ||
        strcmp(body, "application/json") || !r->content_len || r->content_len >= sizeof(body)) return NULL;
    size_t used = 0;
    while (used < r->content_len) {
        int n = httpd_req_recv(r, body + used, r->content_len - used);
        if (n <= 0) return NULL;
        used += (size_t)n;
    }
    body[used] = 0;
    return cJSON_Parse(body);
}

static esp_err_t send_json(httpd_req_t *r, const char *status, cJSON *j)
{
    char *text = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    httpd_resp_set_status(r, status);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_sendstr(r, text ? text : "{}");
    cJSON_free(text);
    return e;
}

// Reply with `ok` on success, otherwise an error object with a status code.
static esp_err_t send_result(httpd_req_t *r, esp_err_t e, const char *status, cJSON *ok)
{
    if (e == ESP_OK) return send_json(r, status, ok ? ok : cJSON_CreateObject());
    cJSON_Delete(ok);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "error", radio_error_name(e));
    cJSON_AddNumberToObject(j, "code", e);
    bool radio = e >= RADIO_ERR_NOT_READY && e <= RADIO_ERR_NOT_PAIRED;
    return send_json(r, e == ESP_ERR_INVALID_ARG ? "400 Bad Request" : e == ESP_ERR_NOT_FOUND ? "404 Not Found" :
                        radio || e == ESP_ERR_INVALID_STATE ? "409 Conflict" : "500 Internal Server Error", j);
}

static const char *string(cJSON *j, const char *key)
{
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, key));
}

static bool integer(cJSON *j, const char *key, double low, double high, double *out)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(j, key);
    if (!cJSON_IsNumber(v) || v->valuedouble < low || v->valuedouble > high || (int64_t)v->valuedouble != v->valuedouble)
        return false;
    *out = v->valuedouble;
    return true;
}

static bool address(cJSON *j, const char *key, uint8_t out[4])
{
    size_t n = 0;
    return rf_parse_hex(string(j, key), out, 4, &n) && n == 4;
}

static void add_address(cJSON *j, const char *key, const uint8_t a[4])
{
    char text[9];
    snprintf(text, sizeof(text), "%02X%02X%02X%02X", a[0], a[1], a[2], a[3]);
    cJSON_AddStringToObject(j, key, text);
}

static void add_report(cJSON *j, const rf_report *report, int64_t age)
{
    cJSON_AddNumberToObject(j, "age_ms", (double)age);
    cJSON_AddBoolToObject(j, "stale", age >= RF_REPORT_STALE_MS);
    add_address(j, "controller", report->destination);
    cJSON_AddNumberToObject(j, "mode", report->mode);
    if (report->temperature_available) cJSON_AddNumberToObject(j, "temperature_c", report->temperature_centi_c / 100.0);
    else cJSON_AddNullToObject(j, "temperature_c");
    if (report->target_available) cJSON_AddNumberToObject(j, "target_c", report->target_centi_c / 100.0);
    else cJSON_AddNullToObject(j, "target_c");
}

static cJSON *radiator_json(const radiator_record *d, int64_t now)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "id", d->id);
    cJSON_AddStringToObject(j, "name", d->name);
    add_address(j, "radiator", d->radiator);
    bool paired = radiator_paired(d);
    cJSON_AddBoolToObject(j, "paired", paired);
    if (paired) add_address(j, "controller", d->controller);
    if (d->has_report) add_report(cJSON_AddObjectToObject(j, "observed"), &d->report, now - d->report_ms);
    if (paired) {
        cJSON *c = cJSON_AddObjectToObject(j, "command");
        cJSON_AddStringToObject(c, "phase", d->command_queued ? "queued" : rf_control_phase_name(d->command.phase));
        cJSON_AddNumberToObject(c, "attempts", d->command.attempts);
        cJSON_AddNumberToObject(c, "error", d->command_error);
        cJSON_AddStringToObject(c, "error_name", radio_error_name(d->command_error));
        if (d->command_queued || d->command.phase != RF_CONTROL_IDLE) {
            cJSON_AddNumberToObject(c, "requested_mode", d->command.mode);
            cJSON_AddNumberToObject(c, "requested_target_c", d->command.centi_c / 100.0);
        }
        if (d->command.phase == RF_CONTROL_MATCHED) cJSON_AddNumberToObject(c, "match_age_ms", (double)(now - d->command.matched_ms));
    }
    return j;
}

static radiator_record records[CLEVERBOX_RADIATOR_CAPACITY];
static radio_observation observations[RADIO_OBSERVATION_CAPACITY];

static esp_err_t radio_get(httpd_req_t *r)
{
    radio_status s = radio_transport_status();
    int64_t now = now_ms();
    cJSON *j = cJSON_CreateObject();
#ifdef CLEVERBOX_HOST
    simulation_status(j);
#endif
    cJSON_AddBoolToObject(j, "ready", s.ready);
    cJSON_AddNumberToObject(j, "error", s.error);
    cJSON_AddStringToObject(j, "error_stage", s.error_stage);
    cJSON_AddNumberToObject(j, "chip_version", s.chip_version);
    cJSON_AddNumberToObject(j, "valid_frames", s.valid_frames);
    cJSON_AddNumberToObject(j, "invalid_frames", s.invalid_frames);
    cJSON_AddNumberToObject(j, "receive_errors", s.receive_errors);
    cJSON_AddNumberToObject(j, "transmissions", s.transmissions);
    cJSON_AddNumberToObject(j, "capacity", CLEVERBOX_RADIATOR_CAPACITY);
    cJSON_AddNumberToObject(j, "registry_error", s.registry_error);
    if (s.has_frame) {
        char hex[RF_MAX_FRAME * 2 + 1];
        rf_to_hex(&s.last_frame, hex);
        cJSON_AddStringToObject(j, "last_frame", hex);
        cJSON_AddNumberToObject(j, "frame_age_ms", (double)(now - s.frame_ms));
    }
    cJSON *p = cJSON_AddObjectToObject(j, "pairing");
    cJSON_AddNumberToObject(p, "id", s.pairing_id);
    cJSON_AddStringToObject(p, "phase", rf_pair_phase_name(s.pairing.phase));
    cJSON_AddNumberToObject(p, "error", s.pairing_error);
    cJSON_AddStringToObject(p, "error_name", radio_error_name(s.pairing_error));
    add_address(p, "controller", s.pairing.controller);
    add_address(p, "discovery", s.pairing.candidate);
    add_address(p, "radiator", s.pairing.radiator);
    cJSON *list = cJSON_AddArrayToObject(j, "radiators");
    size_t count = radio_radiators(records, CLEVERBOX_RADIATOR_CAPACITY);
    for (size_t i = 0; i < count; ++i) cJSON_AddItemToArray(list, radiator_json(&records[i], now));
    list = cJSON_AddArrayToObject(j, "observations");
    size_t heard = radio_observations(observations, RADIO_OBSERVATION_CAPACITY);
    for (size_t i = 0; i < heard; ++i) {
        cJSON *o = cJSON_CreateObject();
        add_address(o, "radiator", observations[i].report.source);
        add_report(o, &observations[i].report, now - observations[i].time_ms);
        for (size_t k = 0; k < count; ++k)
            if (!memcmp(records[k].radiator, observations[i].report.source, 4)) cJSON_AddNumberToObject(o, "id", records[k].id);
        cJSON_AddItemToArray(list, o);
    }
    return send_json(r, "200 OK", j);
}

static esp_err_t radio_post(httpd_req_t *r)
{
    cJSON *j = read_json(r);
    esp_err_t e = j && string(j, "action") && !strcmp(string(j, "action"), "retry") ? radio_retry() : ESP_ERR_INVALID_ARG;
    cJSON_Delete(j);
    return send_result(r, e, "202 Accepted", NULL);
}

static esp_err_t pairing_post(httpd_req_t *r)
{
    cJSON *j = read_json(r);
    const char *action = string(j, "action");
    uint8_t controller[4], discovery[4];
    uint32_t id = 0;
    esp_err_t e = ESP_ERR_INVALID_ARG;
    if (action && !strcmp(action, "start") && address(j, "source", controller) && address(j, "destination", discovery))
        e = radio_pair_start(controller, discovery, &id);
    else if (action && !strcmp(action, "cancel")) e = radio_pair_cancel();
    cJSON_Delete(j);
    cJSON *ok = cJSON_CreateObject();
    if (id) cJSON_AddNumberToObject(ok, "id", id);
    return send_result(r, e, "202 Accepted", ok);
}

static esp_err_t command_post(httpd_req_t *r)
{
    cJSON *j = read_json(r);
    double id, mode, centi_c;
    esp_err_t e = integer(j, "id", 1, UINT32_MAX, &id) && integer(j, "mode", 0, 3, &mode) &&
                  integer(j, "centi_c", 0, 3000, &centi_c)
                  ? radio_command((uint32_t)id, (rf_mode)mode, (int)centi_c, 0) : ESP_ERR_INVALID_ARG;
    cJSON_Delete(j);
    return send_result(r, e, "202 Accepted", NULL);
}

static esp_err_t radiators_post(httpd_req_t *r)
{
    cJSON *j = read_json(r);
    const char *action = string(j, "action");
    double id = 0;
    uint32_t added = 0;
    uint8_t radiator[4];
    esp_err_t e = ESP_ERR_INVALID_ARG;
    if (!action) {
    } else if (!strcmp(action, "monitor") && address(j, "radiator", radiator)) {
        e = radio_monitor(radiator, string(j, "name"), &added);
    } else if (integer(j, "id", 1, UINT32_MAX, &id)) {
        if (!strcmp(action, "rename")) e = radio_rename((uint32_t)id, string(j, "name"));
        else if (!strcmp(action, "remove")) e = radio_remove((uint32_t)id);
    }
    cJSON_Delete(j);
    cJSON *ok = cJSON_CreateObject();
    cJSON_AddNumberToObject(ok, "id", added ? added : (uint32_t)id);
    return send_result(r, e, "200 OK", ok);
}

// Builds a short frame without sending it.
static esp_err_t preview_post(httpd_req_t *r)
{
    cJSON *j = read_json(r);
    const char *command = string(j, "command");
    uint8_t source[4], destination[4];
    rf_frame f = {0};
    bool ok = command && address(j, "source", source) && address(j, "destination", destination) &&
              (!strcmp(command, "offer") || !strcmp(command, "bind") || !strcmp(command, "poll")) &&
              rf_short(&f, source, destination, !strcmp(command, "offer") ? RF_OFFER : !strcmp(command, "bind") ? RF_BIND : RF_POLL);
    cJSON_Delete(j);
    cJSON *out = cJSON_CreateObject();
    if (ok) {
        char hex[RF_MAX_FRAME * 2 + 1];
        rf_to_hex(&f, hex);
        cJSON_AddStringToObject(out, "frame", hex);
    }
    return send_result(r, ok ? ESP_OK : ESP_ERR_INVALID_ARG, "200 OK", out);
}

static esp_err_t network_get(httpd_req_t *r)
{
    network_status s;
    network_get_status(&s);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "state", network_state_name(s.state));
    cJSON_AddStringToObject(j, "ip", s.ip);
    cJSON_AddBoolToObject(j, "configured", s.configured);
    cJSON_AddBoolToObject(j, "ap", s.ap);
    cJSON_AddNumberToObject(j, "error", s.error);
    return send_json(r, "200 OK", j);
}

static esp_err_t network_post(httpd_req_t *r)
{
    cJSON *j = read_json(r);
    const char *action = string(j, "action"), *ssid = string(j, "ssid"), *password = string(j, "password");
    network_credentials c = {0};
    esp_err_t e = ESP_ERR_INVALID_ARG;
    if (!action) {
    } else if (!strcmp(action, "configure") && ssid && password &&
               strlen(ssid) < sizeof(c.ssid) && strlen(password) < sizeof(c.password)) {
        strcpy(c.ssid, ssid); strcpy(c.password, password);
        e = network_configure(&c);
    } else if (!strcmp(action, "forget")) e = network_forget();
    else if (!strcmp(action, "finish_recovery")) e = network_close_ap();
    cJSON_Delete(j);
    return send_result(r, e, "200 OK", NULL);
}

static esp_err_t time_get(httpd_req_t *r)
{
    device_time_status s;
    device_time_get(&s);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "usable", s.usable);
    cJSON_AddStringToObject(j, "source", s.source == TIME_NETWORK ? "network" : s.source == TIME_MANUAL ? "manual" : "unset");
    cJSON_AddStringToObject(j, "timezone", s.timezone);
    if (s.usable) {
        cJSON_AddNumberToObject(j, "unix_seconds", (double)s.unix_seconds);
        cJSON_AddStringToObject(j, "utc", s.utc);
        cJSON_AddStringToObject(j, "local", s.local);
    }
    return send_json(r, "200 OK", j);
}

static esp_err_t time_post(httpd_req_t *r)
{
    cJSON *j = read_json(r);
    const char *action = string(j, "action");
    double seconds;
    esp_err_t e = ESP_ERR_INVALID_ARG;
    if (action && !strcmp(action, "timezone")) e = device_time_timezone(string(j, "timezone"));
    else if (action && !strcmp(action, "manual") && integer(j, "unix_seconds", DEVICE_TIME_MIN, DEVICE_TIME_MAX, &seconds))
        e = device_time_manual((int64_t)seconds);
    cJSON_Delete(j);
    return send_result(r, e, "200 OK", NULL);
}

static esp_err_t mqtt_get(httpd_req_t *r)
{
    mqtt_bridge_status s;
    mqtt_bridge_get(&s);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "configured", s.configured);
    cJSON_AddBoolToObject(j, "enabled", s.enabled);
    cJSON_AddBoolToObject(j, "connected", s.connected);
    cJSON_AddBoolToObject(j, "discovery_enabled", s.discovery);
    cJSON_AddStringToObject(j, "bridge_id", s.bridge_id);
    cJSON_AddStringToObject(j, "host", s.host);
    cJSON_AddNumberToObject(j, "port", s.port);
    cJSON_AddStringToObject(j, "username", s.username);
    cJSON_AddNumberToObject(j, "error", s.error);
    return send_json(r, "200 OK", j);
}

static esp_err_t mqtt_post(httpd_req_t *r)
{
    cJSON *j = read_json(r);
    const char *action = string(j, "action"), *host = string(j, "host");
    const char *user = string(j, "username"), *password = string(j, "password");
    double port;
    mqtt_settings s = {0};
    esp_err_t e = ESP_ERR_INVALID_ARG;
    if (action && !strcmp(action, "configure")) {
        if (host && user && password && integer(j, "port", 1, 65535, &port) &&
            cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(j, "enabled")) && strlen(host) <= MQTT_HOST_BYTES &&
            strlen(user) <= MQTT_USER_BYTES && strlen(password) <= MQTT_PASSWORD_BYTES) {
            s.enabled = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "enabled"));
            s.port = (uint16_t)port;
            strcpy(s.host, host); strcpy(s.username, user); strcpy(s.password, password);
            e = mqtt_bridge_configure(&s);
        }
    } else if (action) e = mqtt_bridge_action(action);
    cJSON_Delete(j);
    return send_result(r, e, "200 OK", NULL);
}

void http_api_register(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        {.uri = "/api/radio", .method = HTTP_GET, .handler = radio_get},
        {.uri = "/api/radio", .method = HTTP_POST, .handler = radio_post},
        {.uri = "/api/pairing", .method = HTTP_POST, .handler = pairing_post},
        {.uri = "/api/command", .method = HTTP_POST, .handler = command_post},
        {.uri = "/api/radiators", .method = HTTP_POST, .handler = radiators_post},
        {.uri = "/api/preview", .method = HTTP_POST, .handler = preview_post},
        {.uri = "/api/network", .method = HTTP_GET, .handler = network_get},
        {.uri = "/api/network", .method = HTTP_POST, .handler = network_post},
        {.uri = "/api/time", .method = HTTP_GET, .handler = time_get},
        {.uri = "/api/time", .method = HTTP_POST, .handler = time_post},
        {.uri = "/api/mqtt", .method = HTTP_GET, .handler = mqtt_get},
        {.uri = "/api/mqtt", .method = HTTP_POST, .handler = mqtt_post},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i)
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &routes[i]));
}
