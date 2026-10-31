// SPDX-License-Identifier: MIT
#include <math.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "radiator_protocol.h"
#include "radio_transport.h"
#include "http_api.h"
#include "http_access.h"
#include "network_setup.h"
#include "device_time.h"
#include "mqtt_bridge.h"
#ifdef CLEVERBOX_HOST
#include "simulation.h"
#endif

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

// Missing identity is legacy single-radiator resolution only; malformed identity
// never falls back. No server-side browser selection exists.
static bool identity(cJSON *j, uint32_t *id) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(j, "id"); *id = 0;
    if (!v) return true;
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < 1 ||
        v->valuedouble > UINT32_MAX || floor(v->valuedouble) != v->valuedouble) return false;
    *id = (uint32_t)v->valuedouble; return true;
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
    esp_err_t result = ESP_ERR_INVALID_ARG; uint32_t id = 0;
    bool valid_id = identity(j, &id);
    if (valid_id && action && !strcmp(action, "start")) {
        uint8_t source[4], destination[4];
        rf_pairing check = {0};
        if (address(j, "source", source) && address(j, "destination", destination) &&
            rf_pair_start(&check, source, destination, 0))
            result = radio_pair_start(source, destination, &id);
    } else if (valid_id && action && !strcmp(action, "cancel")) result = radio_pair_cancel(id);
    else if (valid_id && action && !strcmp(action, "save")) result = radio_pair_save(id);
    cJSON_Delete(j);
    if (result == RADIO_ERR_LISTEN_ONLY) {
        httpd_resp_set_status(r, "409 Conflict"); return httpd_resp_sendstr(r, "Listen-only mode: enable control explicitly on the station interface first.");
    }
    if (result == ESP_ERR_INVALID_ARG)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Invalid pairing action or addresses");
    if (result != ESP_OK) {
        httpd_resp_set_status(r, "409 Conflict");
        return httpd_resp_sendstr(r, "Radio unavailable, exchange busy, registry full, or storage blocked. Check radio status.");
    }
    httpd_resp_set_status(r, "202 Accepted");
    httpd_resp_set_type(r, "application/json");
    char response[64]; snprintf(response, sizeof(response), "{\"accepted\":true,\"id\":%u}", (unsigned)id);
    return httpd_resp_sendstr(r, response);
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
    int mode, target, clock = 0; uint32_t id = 0;
    bool override = cJSON_GetObjectItemCaseSensitive(j, "clock_seconds") != NULL;
#ifndef CLEVERBOX_HOST
    if (override) {
        cJSON_Delete(j);
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST,
            "clock_seconds is host-only; physical commands require device time");
    }
#endif
    bool valid = identity(j, &id) && integer(j, "mode", 0, 3, &mode) && integer(j, "centi_c", 0, 3000, &target) &&
                 (!override || integer(j, "clock_seconds", 0, 86399, &clock));
    // Omission selects learned RAM state, then saved state. Explicit invalid imports must
    // fail, rather than silently falling back to a different template.
    if (valid && cJSON_GetObjectItemCaseSensitive(j, "state"))
        valid = rf_parse_hex(string(j, "state"), command.state.bytes,
                             sizeof(command.state.bytes), &command.state.size);
    if (valid) {
        command.mode = (rf_mode)mode; command.centi_c = target; command.clock_seconds = clock;
    }
    cJSON_Delete(j);
    esp_err_t result = ESP_ERR_INVALID_ARG;
    if (valid) {
#ifdef CLEVERBOX_HOST
        result = override ? radio_control_start(id, &command) : radio_control_start_device(id, &command);
#else
        result = radio_control_start_device(id, &command);
#endif
    }
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    if (result == RADIO_ERR_LISTEN_ONLY) {
        httpd_resp_set_status(r, "409 Conflict"); return httpd_resp_sendstr(r, "Listen-only mode or mode storage error: no RF commands admitted.");
    }
    if (result == RADIO_ERR_TIME_UNAVAILABLE) {
        httpd_resp_set_status(r, "409 Conflict");
        return httpd_resp_sendstr(r, "Device time unavailable. Connect home Wi-Fi for SNTP or explicitly set UTC time in Device time, then retry.");
    }
    if (result == ESP_ERR_INVALID_ARG)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Invalid target, mode, clock or radiator template");
    if (result != ESP_OK) {
        httpd_resp_set_status(r, "409 Conflict");
        return httpd_resp_sendstr(r, "Command busy, radio unavailable, no saved pairing or no learned controller state or saved state; check radio status or import a template");
    }
    httpd_resp_set_status(r, "202 Accepted");
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, "{\"status\":\"queued\"}");
}

static esp_err_t controller_state_action(httpd_req_t *r)
{
    char body[768], type[40];
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
    esp_err_t result = ESP_ERR_INVALID_ARG; uint32_t id = 0;
    bool valid_id = identity(j, &id);
    if (valid_id && action && !strcmp(action, "save")) result = radio_state_save(id, NULL);
    else if (valid_id && action && !strcmp(action, "clear")) result = radio_state_clear(id);
    else if (valid_id && action && !strcmp(action, "adopt")) {
        uint8_t radiator[4];
        cJSON *replace = cJSON_GetObjectItemCaseSensitive(j, "replace");
        if (cJSON_IsObject(j) && cJSON_GetArraySize(j) == 5 && id &&
            address(j, "radiator", radiator) && cJSON_IsBool(replace))
            result = radio_candidate_adopt(id, radiator, string(j, "version"), cJSON_IsTrue(replace));
    }
    else if (valid_id && action && !strcmp(action, "import")) {
        rf_frame frame = {0};
        if (rf_parse_hex(string(j, "state"), frame.bytes, sizeof(frame.bytes), &frame.size))
            result = radio_state_save(id, &frame);
    }
    cJSON_Delete(j);
    if (result == RADIO_ERR_CANDIDATE_CHANGED || result == RADIO_ERR_REPLACE_REQUIRED) {
        httpd_resp_set_status(r, "409 Conflict");
        return httpd_resp_sendstr(r, result == RADIO_ERR_CANDIDATE_CHANGED ?
            "Candidate replaced, evicted or from another boot. Refresh and select again." :
            "Saved template already exists. Explicit replace:true required.");
    }
    if (result == ESP_ERR_INVALID_ARG)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Invalid action or same-radiator controller-state frame");
    if (result != ESP_OK) {
        httpd_resp_set_status(r, "409 Conflict");
        return httpd_resp_sendstr(r, "No saved pair, no learned state, or action busy; check radio status");
    }
    httpd_resp_set_status(r, "202 Accepted");
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, "{\"accepted\":true,\"transmitted\":false}");
}

static const char *store_result(radio_store_result result)
{
    switch (result) {
        case RADIO_STORE_PENDING: return "pending";
        case RADIO_STORE_OK: return "succeeded";
        case RADIO_STORE_FAILED: return "failed";
        default: return "not_attempted";
    }
}

static void address_text(const uint8_t a[4], char out[9])
{
    snprintf(out, 9, "%02X%02X%02X%02X", a[0], a[1], a[2], a[3]);
}

static void device_json(cJSON *j, const radiator_record *d, int64_t now) {
    cJSON_AddNumberToObject(j, "id", d->id); cJSON_AddStringToObject(j, "name", d->name);
    char controller[9], radiator[9]; address_text(d->pair.controller, controller); address_text(d->pair.radiator, radiator);
    cJSON *pair = cJSON_AddObjectToObject(j, "pairing");
    cJSON_AddBoolToObject(pair, "saved", true); cJSON_AddStringToObject(pair, "phase", "complete");
    cJSON_AddNumberToObject(pair, "error", 0);
    cJSON_AddStringToObject(pair, "controller", controller); cJSON_AddStringToObject(pair, "radiator", radiator);
    cJSON *command = cJSON_AddObjectToObject(j, "command");
    cJSON_AddStringToObject(command, "phase", rf_control_phase_name(d->control.phase));
    cJSON_AddNumberToObject(command, "error", d->control_error);
    cJSON_AddNumberToObject(command, "attempts", d->control.attempts);
    cJSON_AddBoolToObject(command, "pending", d->command_reserved);
    cJSON_AddBoolToObject(command, "template_available", d->has_control_template || d->has_saved_template);
    cJSON_AddStringToObject(command, "template_source", d->has_control_template ? "learned" :
                           d->has_saved_template ? "saved" : "missing");
    cJSON_AddBoolToObject(command, "learned_available", d->has_control_template);
    cJSON_AddBoolToObject(command, "saved_available", d->has_saved_template);
    cJSON_AddNumberToObject(command, "state_load_error", d->state_load_error);
    cJSON_AddNumberToObject(command, "state_store_error", d->state_store_error);
    cJSON_AddStringToObject(command, "save_status", store_result(d->state_save_result));
    cJSON_AddStringToObject(command, "clear_status", store_result(d->state_clear_result));
    if (d->has_control_template)
        cJSON_AddNumberToObject(command, "template_age_ms", (now - d->control_template_time_us) / 1000);
    if (d->control.phase != RF_CONTROL_IDLE) {
        cJSON_AddNumberToObject(command, "requested_target_c", d->control.centi_c / 100.0);
        cJSON_AddNumberToObject(command, "requested_mode", d->control.mode);
    }
    if (d->control.phase == RF_CONTROL_MATCHED)
        cJSON_AddNumberToObject(command, "match_age_ms", now / 1000 - d->control.matched_ms);
    if (d->has_paired_report) {
        cJSON *observed = cJSON_AddObjectToObject(j, "observed");
        int64_t age = (now - d->paired_report_time_us) / 1000;
        cJSON_AddNumberToObject(observed, "age_ms", age);
        cJSON_AddBoolToObject(observed, "stale", age >= RF_REPORT_STALE_MS);
        cJSON_AddNumberToObject(observed, "mode", d->paired_report.mode);
        if (d->paired_report.temperature_available)
            cJSON_AddNumberToObject(observed, "temperature_c", d->paired_report.temperature_centi_c / 100.0);
        else cJSON_AddNullToObject(observed, "temperature_c");
        if (d->paired_report.target_available)
            cJSON_AddNumberToObject(observed, "target_c", d->paired_report.target_centi_c / 100.0);
        else cJSON_AddNullToObject(observed, "target_c");
    }
}
static esp_err_t json_response(httpd_req_t *r, cJSON *j) {
    char *body = cJSON_PrintUnformatted(j); cJSON_Delete(j);
    if (!body) return ESP_ERR_NO_MEM;
    httpd_resp_set_type(r, "application/json"); httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    esp_err_t result = httpd_resp_sendstr(r, body); cJSON_free(body); return result;
}
// Passive diagnostics deliberately have the same AP/unknown read exposure as
// existing RF diagnostics. Candidate bytes remain internal; mutations need STA.
static void matching_pair(cJSON *j, const uint8_t radiator[4]) {
    radiator_record d; bool found = false;
    for (size_t i = 0; radio_device_at(i, &d) == ESP_OK; ++i)
        if (!memcmp(d.pair.radiator, radiator, 4)) { found = true; break; }
    cJSON_AddBoolToObject(j, "paired", found);
    cJSON_AddBoolToObject(j, "saved_template", found && d.has_saved_template);
    if (found) cJSON_AddNumberToObject(j, "paired_id", d.id);
}
static esp_err_t commissioning_status(httpd_req_t *r) {
    radio_status s = radio_transport_status(); int64_t now = esp_timer_get_time();
    cJSON *j = cJSON_CreateObject(); if (!j) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(j, "mode", s.control_enabled ? "control-enabled" : "listen-only");
    cJSON_AddBoolToObject(j, "pending", s.mode_pending);
    cJSON_AddStringToObject(j, "save_status", store_result(s.mode_save_result));
    cJSON_AddNumberToObject(j, "storage_error", s.mode_error);
    cJSON_AddBoolToObject(j, "persistence_uncertain", s.mode_persistence_uncertain);
    cJSON_AddNumberToObject(j, "capacity", RADIO_PASSIVE_CAPACITY);
    cJSON_AddBoolToObject(j, "observations_full", s.observation_count == RADIO_PASSIVE_CAPACITY);
    cJSON_AddBoolToObject(j, "candidates_full", s.candidate_count == RADIO_PASSIVE_CAPACITY);
    cJSON_AddNumberToObject(j, "observation_evictions", s.observation_evictions);
    cJSON_AddNumberToObject(j, "candidate_evictions", s.candidate_evictions);
    cJSON *observed = cJSON_AddArrayToObject(j, "observations");
    radio_observation o;
    for (size_t i = 0; radio_observation_at(i, &o) == ESP_OK; ++i) {
        cJSON *row = cJSON_CreateObject(); char a[9], b[9];
        address_text(o.report.source, a); address_text(o.report.destination, b);
        cJSON_AddStringToObject(row, "radiator", a); cJSON_AddStringToObject(row, "controller", b);
        int64_t age = (now - o.time_us) / 1000;
        cJSON_AddNumberToObject(row, "age_ms", age); cJSON_AddBoolToObject(row, "stale", age >= RF_REPORT_STALE_MS);
        cJSON_AddNumberToObject(row, "mode", o.report.mode);
        if (o.report.temperature_available) cJSON_AddNumberToObject(row, "temperature_c", o.report.temperature_centi_c / 100.0);
        else cJSON_AddNullToObject(row, "temperature_c");
        if (o.report.target_available) cJSON_AddNumberToObject(row, "target_c", o.report.target_centi_c / 100.0);
        else cJSON_AddNullToObject(row, "target_c");
        matching_pair(row, o.report.source); cJSON_AddItemToArray(observed, row);
    }
    cJSON *captured = cJSON_AddArrayToObject(j, "candidates");
    radio_candidate c;
    for (size_t i = 0; radio_candidate_at(i, &c) == ESP_OK; ++i) {
        cJSON *row = cJSON_CreateObject(); char a[9], b[9];
        address_text(c.state + 6, a); address_text(c.state + 1, b);
        cJSON_AddStringToObject(row, "radiator", a); cJSON_AddStringToObject(row, "controller", b);
        cJSON_AddStringToObject(row, "version", c.version);
        cJSON_AddNumberToObject(row, "age_ms", (now - c.time_us) / 1000);
        matching_pair(row, c.state + 6); cJSON_AddItemToArray(captured, row);
    }
    cJSON *adoption = cJSON_AddObjectToObject(j, "adoption");
    cJSON_AddNumberToObject(adoption, "id", s.adoption_id);
    cJSON_AddStringToObject(adoption, "version", s.adoption_version);
    cJSON_AddStringToObject(adoption, "status", store_result(s.adoption_result));
    cJSON_AddNumberToObject(adoption, "error", s.adoption_error);
    return json_response(r, j);
}
static esp_err_t commissioning_action(httpd_req_t *r) {
    char body[128], type[40];
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
    cJSON *j = !memchr(body, 0, used) && mqtt_json_flat(body) ? cJSON_ParseWithOpts(body, NULL, true) : NULL;
    const char *mode = string(j, "mode");
    esp_err_t e = ESP_ERR_INVALID_ARG;
    if (cJSON_IsObject(j) && cJSON_GetArraySize(j) == 1 && mode &&
        (!strcmp(mode, "listen-only") || !strcmp(mode, "control-enabled")))
        e = radio_mode_set(!strcmp(mode, "control-enabled"));
    cJSON_Delete(j);
    if (e == ESP_ERR_INVALID_ARG) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST,
        "Use mode: listen-only or control-enabled");
    if (e != ESP_OK) {
        httpd_resp_set_status(r, "409 Conflict");
        return httpd_resp_sendstr(r, "Mode transition busy: finish pairing, outstanding RF commands or local persistence first; then retry explicitly.");
    }
    httpd_resp_set_status(r, "202 Accepted"); httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, "{\"accepted\":true,\"transmitted\":false}");
}
static esp_err_t radio_status_page(httpd_req_t *r) {
    radio_status s = radio_transport_status(); int64_t now = esp_timer_get_time();
    uint32_t id = 0; char query[64];
    if (httpd_req_get_url_query_len(r)) {
        if (httpd_req_get_url_query_str(r, query, sizeof(query)) != ESP_OK || strncmp(query, "id=", 3))
            return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Use ?id=<stable identity>");
        char *end; errno = 0; unsigned long long parsed = strtoull(query + 3, &end, 10);
        if (errno || !query[3] || *end || parsed == 0 || parsed > UINT32_MAX || query[3] < '0' || query[3] > '9')
            return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Invalid identity");
        id = (uint32_t)parsed;
    }
    radiator_record selected = {0};
    bool has_device = radio_device_status(id, &selected) == ESP_OK;
    if (id && !has_device) { httpd_resp_set_status(r, "404 Not Found"); return httpd_resp_sendstr(r, "Unknown radiator identity"); }
    cJSON *j = cJSON_CreateObject(); if (!j) return ESP_ERR_NO_MEM;
#ifdef CLEVERBOX_HOST
    simulation_status(j);
#endif
    cJSON_AddBoolToObject(j, "ready", s.ready); cJSON_AddNumberToObject(j, "error", s.error);
    cJSON_AddNumberToObject(j, "chip_version", s.chip_version);
    cJSON_AddNumberToObject(j, "valid_frames", s.valid_frames); cJSON_AddNumberToObject(j, "invalid_frames", s.invalid_frames);
    cJSON_AddNumberToObject(j, "receive_errors", s.receive_errors);
    cJSON_AddNumberToObject(j, "capacity", s.capacity); cJSON_AddNumberToObject(j, "registry_error", s.registry_error);
    cJSON_AddStringToObject(j, "registry_problem", s.registry_error == DEVICE_STORE_ERR_CAPACITY ?
        "stored_registry_exceeds_capacity" : s.registry_error ? "load_or_migration_failed" : "none");
    cJSON_AddNumberToObject(j, "exchange_id", s.exchange_id);
    cJSON_AddBoolToObject(j, "mutation_pending", s.mutation_pending); cJSON_AddNumberToObject(j, "mutation_error", s.mutation_error);
    cJSON *devices = cJSON_AddArrayToObject(j, "radiators"); radiator_record d;
    unsigned active_commands = 0;
    for (size_t i = 0; radio_device_at(i, &d) == ESP_OK; ++i) {
        if (d.command_reserved) ++active_commands;
        cJSON *item = cJSON_CreateObject(); device_json(item, &d, now); cJSON_AddItemToArray(devices, item);
    }
    cJSON_AddNumberToObject(j, "active_commands", active_commands);
    if (has_device) device_json(j, &selected, now);
    else if (s.device_count == 0) { radiator_record empty = {0}; device_json(j, &empty, now); cJSON_DeleteItemFromObjectCaseSensitive(j, "pairing"); }
    if (!has_device) {
        cJSON *pair = cJSON_AddObjectToObject(j, "pairing");
        cJSON_AddStringToObject(pair, "phase", rf_pair_phase_name(s.pairing.phase));
        cJSON_AddBoolToObject(pair, "saved", s.device_count != 0 && s.pairing_saved); cJSON_AddNumberToObject(pair, "error", s.pairing_error);
        char controller[9], radiator[9]; address_text(s.pairing.controller, controller); address_text(s.pairing.radiator, radiator);
        cJSON_AddStringToObject(pair, "controller", controller); cJSON_AddStringToObject(pair, "radiator", radiator);
    }
    cJSON *inclusion = cJSON_AddObjectToObject(j, "inclusion");
    cJSON_AddNumberToObject(inclusion, "id", s.pairing_id);
    cJSON_AddBoolToObject(inclusion, "duplicate", s.pairing_duplicate);
    cJSON_AddStringToObject(inclusion, "phase", rf_pair_phase_name(s.pairing.phase));
    cJSON_AddBoolToObject(inclusion, "saved", s.pairing_saved); cJSON_AddNumberToObject(inclusion, "error", s.pairing_error);
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
    return json_response(r, j);
}
static esp_err_t registry_action(httpd_req_t *r) {
    char body[256], type[40];
    if (httpd_req_get_hdr_value_str(r, "Content-Type", type, sizeof(type)) != ESP_OK ||
        strcmp(type, "application/json") || !r->content_len || r->content_len >= sizeof(body))
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Use a small JSON request");
    size_t used = 0;
    while (used < r->content_len) { int n = httpd_req_recv(r, body + used, r->content_len - used); if (n <= 0) return ESP_FAIL; used += n; }
    body[used] = 0; cJSON *j = cJSON_ParseWithOpts(body, NULL, true);
    uint32_t id = 0; const char *action = string(j, "action"); esp_err_t result = ESP_ERR_INVALID_ARG;
    if (identity(j, &id) && action) {
        if (!strcmp(action, "rename")) result = radio_device_rename(id, string(j, "name"));
        else if (!strcmp(action, "remove")) result = radio_device_remove(id);
        else if (!strcmp(action, "select")) {
            radiator_record d; result = radio_device_status(id, &d);
            if (result == ESP_OK) {
                cJSON_Delete(j); cJSON *out = cJSON_CreateObject(); device_json(out, &d, esp_timer_get_time());
                return json_response(r, out); // Stateless selection; caller keeps the identity.
            }
        }
    }
    cJSON_Delete(j);
    if (result == ESP_ERR_INVALID_ARG) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Invalid registry action, identity or name");
    if (result != ESP_OK) { httpd_resp_set_status(r, "409 Conflict"); return httpd_resp_sendstr(r, "Unknown/ambiguous identity, active operation, storage blocked or busy"); }
    httpd_resp_set_status(r, "202 Accepted"); httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, "{\"accepted\":true,\"transmitted\":false}");
}

static esp_err_t network_status_page(httpd_req_t *r) {
    network_status s;
    if (network_get_status(&s) != ESP_OK) {
        httpd_resp_set_status(r, "503 Service Unavailable");
        return httpd_resp_sendstr(r, "Network status unavailable");
    }
    cJSON *j = cJSON_CreateObject();
    http_interface interface = http_request_interface(r);
    cJSON_AddStringToObject(j, "http_interface", http_interface_name(interface));
    cJSON_AddBoolToObject(j, "http_control_allowed", interface == HTTP_INTERFACE_STA);
    cJSON_AddBoolToObject(j, "ap_closing", s.ap_closing);
    cJSON_AddStringToObject(j, "state", network_state_name(s.state));
    cJSON_AddStringToObject(j, "setup_result", network_setup_name(s.setup));
    cJSON_AddBoolToObject(j, "configured", s.configured);
    cJSON_AddBoolToObject(j, "persistence_uncertain", s.persistence_uncertain);
    cJSON_AddBoolToObject(j, "recovery_ap", s.recovery_ap);
    cJSON_AddBoolToObject(j, "manual_recovery", s.manual_recovery);
    cJSON_AddStringToObject(j, "recovery_error", s.recovery_error);
    cJSON_AddNumberToObject(j, "event_queue_overflows", s.event_queue_overflows);
    cJSON_AddNumberToObject(j, "attempts", s.attempts);
    cJSON_AddStringToObject(j, "ip", s.ip);
    cJSON_AddStringToObject(j, "error", s.error);
    return json_response(r, j);
}
static esp_err_t network_action(httpd_req_t *r) {
    char body[768], type[40];
    if (httpd_req_get_hdr_value_str(r, "Content-Type", type, sizeof(type)) != ESP_OK ||
        strcmp(type, "application/json") || !r->content_len || r->content_len >= sizeof(body))
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Use a small JSON request");
    size_t used = 0;
    while (used < r->content_len) {
        int n = httpd_req_recv(r, body + used, r->content_len - used);
        if (n <= 0) { memset(body, 0, sizeof(body)); return ESP_FAIL; }
        used += (size_t)n;
    }
    body[used] = 0;
    // cJSON strings are NUL terminated. Reject actual escaped/embedded NUL,
    // while allowing literal backslash text in otherwise valid credentials.
    bool valid = !memchr(body, 0, used);
    for (size_t i = 0; valid && i + 1 < used; ++i) {
        if (body[i] != '\\') continue;
        if (i + 5 < used && !strncmp(body + i + 1, "u0000", 5)) valid = false;
        ++i; // Skip the escaped character, including an escaped backslash.
    }
    cJSON *j = valid ? cJSON_ParseWithOpts(body, NULL, true) : NULL;
    memset(body, 0, sizeof(body));
    const char *action = string(j, "action");
    esp_err_t err = ESP_ERR_INVALID_ARG;
    network_credentials credentials = {0};
    const char *ssid = string(j, "ssid"), *password = string(j, "password");
    if (cJSON_IsObject(j) && action && !strcmp(action, "configure") &&
        cJSON_GetArraySize(j) == 3 && ssid && password &&
        strlen(ssid) < sizeof(credentials.ssid) && strlen(password) < sizeof(credentials.password)) {
        strcpy(credentials.ssid, ssid); strcpy(credentials.password, password);
        err = network_configure(&credentials);
    } else if (cJSON_IsObject(j) && action && !strcmp(action, "forget") && cJSON_GetArraySize(j) == 2 &&
               cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "confirm")))
        err = network_forget();
    else if (cJSON_IsObject(j) && action && !strcmp(action, "finish_recovery") && cJSON_GetArraySize(j) == 1)
        err = network_finish_recovery();
    // Avoid retaining the password in the parsed request after admission.
    cJSON *secret = cJSON_GetObjectItemCaseSensitive(j, "password");
    if (cJSON_IsString(secret)) memset(secret->valuestring, 0, strlen(secret->valuestring));
    memset(&credentials, 0, sizeof(credentials)); cJSON_Delete(j);
    if (err == ESP_ERR_INVALID_ARG)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Use configure with SSID (1–32 bytes) and personal-network password (8–63 printable ASCII characters), finish_recovery, or forget with confirm:true");
    if (err != ESP_OK) {
        httpd_resp_set_status(r, "409 Conflict");
        return httpd_resp_sendstr(r, "Network setup busy or unavailable; check status and retry");
    }
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_set_type(r, "application/json"); httpd_resp_set_status(r, "202 Accepted");
    return httpd_resp_sendstr(r, "{\"accepted\":true}");
}

static esp_err_t time_status_page(httpd_req_t *r) {
    device_time_status s; device_time_get(&s);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "usable", s.usable);
    cJSON_AddStringToObject(j, "source", s.source == TIME_NETWORK ? "network" : s.source == TIME_MANUAL ? "manual" : "unsynchronized");
    cJSON_AddStringToObject(j, "timezone", s.timezone);
    cJSON_AddBoolToObject(j, "synchronized_this_boot", s.synchronized_this_boot);
    cJSON_AddBoolToObject(j, "network_connected", s.network_connected);
    if (s.synchronized_this_boot) cJSON_AddNumberToObject(j, "sync_age_ms", s.sync_age_ms);
    else cJSON_AddNullToObject(j, "sync_age_ms");
    cJSON_AddNumberToObject(j, "config_error", s.config_error);
    cJSON_AddNumberToObject(j, "sntp_error", s.sntp_error);
    cJSON_AddBoolToObject(j, "persistence_uncertain", s.persistence_uncertain);
    if (s.usable) {
        cJSON_AddNumberToObject(j, "unix_seconds", (double)s.unix_seconds);
        cJSON_AddStringToObject(j, "utc", s.utc);
        cJSON_AddStringToObject(j, "local", s.local);
    }
    return json_response(r, j);
}
static esp_err_t time_action(httpd_req_t *r) {
    char body[256], type[40];
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
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
    bool valid = !memchr(body, 0, used);
    for (size_t i = 0; valid && i + 1 < used; ++i) {
        if (body[i] != '\\') continue;
        if (i + 5 < used && !strncmp(body + i + 1, "u0000", 5)) valid = false;
        ++i;
    }
    cJSON *j = valid ? cJSON_ParseWithOpts(body, NULL, true) : NULL;
    const char *action = string(j, "action");
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (cJSON_IsObject(j) && cJSON_GetArraySize(j) == 2 && action) {
        if (!strcmp(action, "timezone")) err = device_time_timezone(string(j, "timezone"));
        else if (!strcmp(action, "manual")) {
            cJSON *v = cJSON_GetObjectItemCaseSensitive(j, "unix_seconds");
            if (cJSON_IsNumber(v) && isfinite(v->valuedouble) &&
                v->valuedouble >= DEVICE_TIME_MIN && v->valuedouble <= DEVICE_TIME_MAX &&
                floor(v->valuedouble) == v->valuedouble)
                err = device_time_manual((int64_t)v->valuedouble);
        }
    }
    cJSON_Delete(j);
    if (err == ESP_ERR_INVALID_ARG)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Use timezone with a supported POSIX rule, or manual with integer UTC unix_seconds (2020–2099)");
    if (err != ESP_OK) {
        httpd_resp_set_status(r, "409 Conflict");
        return httpd_resp_sendstr(r, "Time update failed. Check time status; timezone persistence may be uncertain. Retry explicitly.");
    }
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, "{\"updated\":true,\"transmitted\":false}");
}

static esp_err_t mqtt_status_page(httpd_req_t *r) {
    mqtt_bridge_status s; mqtt_bridge_get(&s); cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "configured", s.configured); cJSON_AddBoolToObject(j, "enabled", s.enabled);
    cJSON_AddBoolToObject(j, "connected", s.connected); cJSON_AddBoolToObject(j, "pending", s.pending);
    cJSON_AddBoolToObject(j, "persistence_uncertain", s.persistence_uncertain);
    cJSON_AddStringToObject(j, "bridge_id", s.bridge_id); cJSON_AddStringToObject(j, "session", s.session);
    cJSON_AddStringToObject(j, "host", s.host); cJSON_AddNumberToObject(j, "port", s.port);
    cJSON_AddStringToObject(j, "username", s.username);
    cJSON_AddNumberToObject(j, "config_error", s.config_error); cJSON_AddNumberToObject(j, "client_error", s.client_error);
    cJSON_AddBoolToObject(j, "broker_connected", s.broker_connected);
    cJSON_AddBoolToObject(j, "subscription_pending", s.subscription_pending);
    cJSON_AddNumberToObject(j, "subscription_error", s.subscription_error);
    cJSON_AddBoolToObject(j, "discovery_enabled", s.discovery_enabled);
    cJSON_AddNumberToObject(j, "discovery_error", s.discovery_error);
    cJSON_AddNumberToObject(j, "discovery_pending", s.discovery_pending);
    cJSON_AddNumberToObject(j, "dropped_input", s.dropped_input); return json_response(r, j);
}
static esp_err_t mqtt_action(httpd_req_t *r) {
    char body[768], type[40]; httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    if (httpd_req_get_hdr_value_str(r, "Content-Type", type, sizeof(type)) != ESP_OK ||
        strcmp(type, "application/json") || !r->content_len || r->content_len >= sizeof(body))
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Use a small JSON request");
    size_t used = 0;
    while (used < r->content_len) {
        int n = httpd_req_recv(r, body+used, r->content_len-used);
        if (n <= 0) { memset(body, 0, sizeof(body)); return ESP_FAIL; }
        used += (size_t)n;
    }
    body[used] = 0; bool valid = !memchr(body, 0, used);
    for (size_t i = 0; valid && i+1 < used; ++i) {
        if (body[i] != '\\') continue;
        if (i+5 < used && !strncmp(body+i+1, "u0000", 5)) valid = false;
        ++i;
    }
    cJSON *j = valid && mqtt_json_flat(body) ? cJSON_ParseWithOpts(body, NULL, true) : NULL; memset(body, 0, sizeof(body));
    const char *action = string(j, "action"); esp_err_t err = ESP_ERR_INVALID_ARG;
    mqtt_settings settings = {0}; int port;
    const char *host = string(j, "host"), *user = string(j, "username"), *password = string(j, "password");
    cJSON *enabled = cJSON_GetObjectItemCaseSensitive(j, "enabled");
    if (cJSON_IsObject(j) && action && !strcmp(action, "configure") && cJSON_GetArraySize(j) == 6 &&
        host && user && password && strlen(host) <= MQTT_HOST_BYTES && strlen(user) <= MQTT_USER_BYTES &&
        strlen(password) <= MQTT_PASSWORD_BYTES && integer(j, "port", 1, 65535, &port) && cJSON_IsBool(enabled)) {
        settings.configured = true; settings.enabled = cJSON_IsTrue(enabled); settings.port = (uint16_t)port;
        strcpy(settings.host, host); strcpy(settings.username, user); strcpy(settings.password, password);
        err = mqtt_bridge_configure(&settings);
    } else if (cJSON_IsObject(j) && cJSON_GetArraySize(j) == 1) err = mqtt_bridge_action(action);
    cJSON *secret = cJSON_GetObjectItemCaseSensitive(j, "password");
    if (cJSON_IsString(secret)) memset(secret->valuestring, 0, strlen(secret->valuestring));
    memset(&settings, 0, sizeof(settings)); cJSON_Delete(j);
    if (err == ESP_ERR_INVALID_ARG) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST,
        "Use configure with host, port, username, password and enabled, or enable/disable/forget");
    if (err != ESP_OK) { httpd_resp_set_status(r, "409 Conflict"); return httpd_resp_sendstr(r, "MQTT setup busy, or discovery records remain. Disable discovery and finish cleanup before changing or forgetting the broker."); }
    httpd_resp_set_status(r, "202 Accepted"); httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, "{\"accepted\":true,\"transmitted\":false}");
}

// Single endpoint admission table. New routes must deliberately choose their
// recovery exposure. All POSTs except Wi-Fi setup require the trusted LAN.
typedef struct {
    const char *uri;
    int method;
    esp_err_t (*handler)(httpd_req_t *);
    bool recovery;
} api_route;
static const api_route routes[] = {
    {"/api/commissioning", HTTP_GET, commissioning_status, true},
    {"/api/commissioning", HTTP_POST, commissioning_action, false},
    {"/api/mqtt", HTTP_GET, mqtt_status_page, true},
    {"/api/mqtt", HTTP_POST, mqtt_action, false},
    {"/api/time", HTTP_GET, time_status_page, true},
    {"/api/time", HTTP_POST, time_action, false},
    {"/api/network", HTTP_GET, network_status_page, true},
    {"/api/network", HTTP_POST, network_action, true},
    {"/api/radiators", HTTP_GET, radio_status_page, true},
    {"/api/radiators", HTTP_POST, registry_action, false},
    {"/api/preview", HTTP_POST, preview, false},
    {"/api/radio", HTTP_GET, radio_status_page, true},
    {"/api/pairing", HTTP_POST, pairing_action, false},
    {"/api/command", HTTP_POST, live_command, false},
    {"/api/controller-state", HTTP_POST, controller_state_action, false},
};
static esp_err_t dispatch(httpd_req_t *r) {
    const api_route *route = r->user_ctx;
    http_interface interface = http_request_interface(r);
    if (interface == HTTP_INTERFACE_STA ||
        (interface == HTTP_INTERFACE_AP && route->recovery) ||
        (interface == HTTP_INTERFACE_UNKNOWN && route->method == HTTP_GET))
        return route->handler(r);
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    // Close denied POST connections: their unread bodies must not become the
    // next request on a persistent socket. Nothing has reached a handler yet.
    httpd_resp_set_hdr(r, "Connection", "close");
    httpd_resp_set_status(r, "403 Forbidden");
    esp_err_t result = httpd_resp_sendstr(r,
        "This operation requires the station interface on your trusted home LAN. "
        "Recovery access permits Wi-Fi setup and diagnostics only.");
    // IDF closes the session immediately when a URI handler returns ESP_FAIL.
    return result == ESP_OK ? ESP_FAIL : result;
}
void http_api_register(httpd_handle_t server) {
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        httpd_uri_t uri = {.uri = routes[i].uri, .method = routes[i].method,
            .handler = dispatch, .user_ctx = (void *)&routes[i]};
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri));
    }
}
