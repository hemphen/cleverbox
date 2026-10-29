// SPDX-License-Identifier: MIT
#include "mqtt_bridge.h"
#include "radio_transport.h"
#include "network_setup.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "nvs.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// One MQTT task owns settings, client calls, routing and publication. The shared
// lock protects only status/action/event copies, never NVS, JSON or network I/O.
static mqtt_client_ops client;
static mqtt_settings settings, candidate;
static mqtt_bridge_status status;
static bool action_pending, link_connected, running;
static unsigned link_epoch, owner_epoch;
static int64_t retry_ms, state_ms;
static char base[64], availability[96], session[33];
static uint64_t high_water, next_cookie;
typedef struct { char topic[MQTT_TOPIC_BYTES], data[MQTT_PAYLOAD_BYTES+1]; size_t used, total; unsigned epoch; bool retained; } input;
static input assembly, inputs[MQTT_INPUT_SLOTS];
static size_t input_head, input_count;
typedef struct {
    uint32_t id; uint64_t cookie, request_id;
    char session[33]; rf_control_status last; int error; bool published;
} tracking;
static tracking tracked[CLEVERBOX_RADIATOR_CAPACITY];
static int64_t now(void) { return esp_timer_get_time()/1000; }
static void update_settings_status(void) {
    status.configured = settings.configured; status.enabled = settings.enabled;
    strcpy(status.host, settings.host); strcpy(status.username, settings.username); status.port = settings.port;
}
void mqtt_bridge_init(mqtt_client_ops ops, const char bridge_id[13]) {
    client = ops; memset(&settings, 0, sizeof(settings)); memset(&status, 0, sizeof(status));
    memset(tracked, 0, sizeof(tracked)); memset(&assembly, 0, sizeof(assembly));
    action_pending = link_connected = running = false; link_epoch = owner_epoch = 0;
    input_head = input_count = 0; high_water = next_cookie = 0; retry_ms = state_ms = 0; session[0] = 0;
    strcpy(status.bridge_id, bridge_id);
    snprintf(base, sizeof(base), "cleverbox/v1/%s", bridge_id);
    snprintf(availability, sizeof(availability), "%s/availability", base);
    esp_err_t err = mqtt_store_load(&settings);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) status.config_error = err;
    update_settings_status();
}
void mqtt_bridge_get(mqtt_bridge_status *out) { mqtt_platform_lock(); *out = status; mqtt_platform_unlock(); }
esp_err_t mqtt_bridge_configure(const mqtt_settings *s) {
    if (!mqtt_settings_valid(s) || !s->configured) return ESP_ERR_INVALID_ARG;
    mqtt_platform_lock();
    esp_err_t err = status.pending ? ESP_ERR_INVALID_STATE : ESP_OK;
    if (err == ESP_OK) { candidate = *s; action_pending = status.pending = true; }
    mqtt_platform_unlock(); return err;
}
esp_err_t mqtt_bridge_action(const char *action) {
    if (!action) return ESP_ERR_INVALID_ARG;
    mqtt_platform_lock(); esp_err_t err = ESP_OK;
    if (status.pending) err = ESP_ERR_INVALID_STATE;
    else if (!strcmp(action, "forget")) memset(&candidate, 0, sizeof(candidate));
    else if ((!strcmp(action, "enable") || !strcmp(action, "disable")) && settings.configured) {
        candidate = settings; candidate.enabled = !strcmp(action, "enable");
    } else err = ESP_ERR_INVALID_ARG;
    if (err == ESP_OK) action_pending = status.pending = true;
    mqtt_platform_unlock(); return err;
}
void mqtt_bridge_client_error(esp_err_t error) {
    mqtt_platform_lock(); status.client_error = error; mqtt_platform_unlock();
}
void mqtt_bridge_connection(bool connected) {
    mqtt_platform_lock();
    link_connected = connected; ++link_epoch;
    if (connected) status.client_error = ESP_OK;
    status.connected = false; status.session[0] = 0;
    input_head = input_count = 0; assembly.total = 0;
    mqtt_platform_unlock();
}
void mqtt_bridge_input(const char *topic, size_t topic_len, const char *data,
                       size_t length, size_t offset, size_t total, bool retained) {
    mqtt_platform_lock();
    bool valid = link_connected && data && total && total <= MQTT_PAYLOAD_BYTES && offset <= total && length <= total-offset;
    if (!offset) {
        assembly.total = 0;
        valid = valid && topic && topic_len && topic_len < MQTT_TOPIC_BYTES && !memchr(topic, 0, topic_len);
        if (valid) {
            memcpy(assembly.topic, topic, topic_len); assembly.topic[topic_len] = 0;
            assembly.used = 0; assembly.total = total; assembly.epoch = link_epoch; assembly.retained = retained;
        }
    } else valid = valid && assembly.total == total && assembly.used == offset && assembly.epoch == link_epoch;
    if (valid && !memchr(data, 0, length)) {
        memcpy(assembly.data+offset, data, length); assembly.used = offset+length;
        assembly.retained |= retained;
        if (assembly.used == total) {
            assembly.data[total] = 0;
            if (input_count < MQTT_INPUT_SLOTS) {
                inputs[(input_head+input_count)%MQTT_INPUT_SLOTS] = assembly; ++input_count;
            } else ++status.dropped_input;
            assembly.total = 0;
        }
    } else { assembly.total = 0; ++status.dropped_input; }
    mqtt_platform_unlock();
}
static bool publish(const char *suffix, const char *payload, bool retain) {
    char topic[MQTT_TOPIC_BYTES]; snprintf(topic, sizeof(topic), "%s/%s", base, suffix);
    return client.publish(topic, payload, retain);
}
static bool json_publish(const char *suffix, cJSON *j) {
    char payload[768]; bool ok = cJSON_PrintPreallocated(j, payload, sizeof(payload), false);
    cJSON_Delete(j); return ok && publish(suffix, payload, false);
}
static cJSON *envelope(const char *s, uint64_t request_id) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "session", s);
    if (request_id) cJSON_AddNumberToObject(j, "request_id", (double)request_id);
    return j;
}
static void admission(uint32_t id, uint64_t request_id, const char *reason, int error) {
    cJSON *j = envelope(session, request_id); char suffix[64];
    cJSON_AddStringToObject(j, "stage", "admission");
    cJSON_AddBoolToObject(j, "accepted", reason == NULL);
    cJSON_AddStringToObject(j, "reason", reason ? reason : "accepted"); cJSON_AddNumberToObject(j, "error", error);
    if (id) snprintf(suffix, sizeof(suffix), "radiators/%u/result", (unsigned)id);
    else strcpy(suffix, "errors");
    json_publish(suffix, j);
}
static bool number(cJSON *j, const char *key, double low, double high, double *out) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(j, key);
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < low ||
        v->valuedouble > high || floor(v->valuedouble) != v->valuedouble) return false;
    *out = v->valuedouble; return true;
}
// Commands/configuration are flat objects. Bound parser recursion, reject NUL
// escapes (cJSON truncates them), arrays, nested objects and duplicate keys.
bool mqtt_json_flat(const char *p) {
    bool quoted = false; unsigned objects = 0;
    for (; *p; ++p) {
        if (*p == '\\' && quoted) {
            if (!strncmp(p, "\\u0000", 6)) return false;
            if (!p[1]) return false;
            ++p; continue;
        }
        if (*p == '"') quoted = !quoted;
        if (!quoted && (*p == '[' || (*p == '{' && ++objects > 1))) return false;
    }
    return !quoted && objects == 1;
}
static void route(const input *in) {
    size_t n = strlen(base); uint32_t id = 0;
    if (!strncmp(in->topic, base, n) && !strncmp(in->topic+n, "/radiators/", 11)) {
        const char *p = in->topic+n+11; char *end; unsigned long long v = strtoull(p, &end, 10);
        if (*p >= '1' && *p <= '9' && v <= UINT32_MAX && !strcmp(end, "/set")) id = (uint32_t)v;
    }
    if (!id) { admission(0, 0, "invalid_topic", ESP_ERR_INVALID_ARG); return; }
    cJSON *j = mqtt_json_flat(in->data) ? cJSON_ParseWithOpts(in->data, NULL, true) : NULL;
    double seq = 0, mode = 0, target = 0;
    bool has_seq = number(j, "request_id", 1, 9007199254740991.0, &seq);
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(j, "session"));
    const char *reason = NULL; esp_err_t err = ESP_ERR_INVALID_ARG;
    if (in->retained) reason = "retained_command";
    else if (!s || strcmp(s, session)) reason = "session_mismatch";
    else if (!has_seq) reason = "invalid_request_id";
    else if ((uint64_t)seq <= high_water) reason = "duplicate_or_out_of_order";
    else {
        // Consume every new request ID, including rejected requests. No replay,
        // cache eviction or implicit retry. IDs are global within this session.
        high_water = (uint64_t)seq;
        if (!cJSON_IsObject(j) || cJSON_GetArraySize(j) != 4 ||
            !number(j, "mode", 0, 3, &mode) || !number(j, "centi_c", 0, 3000, &target)) reason = "invalid_fields";
        else {
            radiator_record d;
            if (radio_device_status(id, &d) != ESP_OK) reason = "unknown_identity";
            else {
                rf_control_request request = {0}; request.mode = (rf_mode)mode; request.centi_c = (int)target;
                uint64_t cookie = ++next_cookie;
                err = radio_control_start_device_tracked(id, &request, cookie);
                if (err == ESP_OK) {
                    tracking *t = NULL;
                    for (size_t i = 0; i < CLEVERBOX_RADIATOR_CAPACITY; ++i)
                        if (tracked[i].id == id) { t = &tracked[i]; break; }
                    if (!t) for (size_t i = 0; i < CLEVERBOX_RADIATOR_CAPACITY; ++i)
                        if (!tracked[i].id) { t = &tracked[i]; break; }
                    // Removed identities are pruned by observations before routing.
                    if (t) { memset(t, 0, sizeof(*t)); t->id = id; t->cookie = cookie; t->request_id = (uint64_t)seq; strcpy(t->session, session); }
                } else if (err == RADIO_ERR_TIME_UNAVAILABLE) reason = "time_unavailable";
                else if (err == ESP_ERR_INVALID_ARG) reason = "invalid_settings_or_template";
                else reason = "busy_or_unavailable_or_missing_template";
            }
        }
    }
    admission(id, has_seq ? (uint64_t)seq : 0, reason, reason ? err : ESP_OK); cJSON_Delete(j);
}
static void results(bool force) {
    for (size_t i = 0; i < CLEVERBOX_RADIATOR_CAPACITY; ++i) {
        tracking *t = &tracked[i]; if (!t->id) continue;
        radiator_record d;
        if (radio_device_status(t->id, &d) != ESP_OK) { memset(t, 0, sizeof(*t)); continue; }
        if (d.result_cookie != t->cookie) continue;
        rf_control_status *r = &d.tracked_result;
        if (!force && t->published && t->last.phase == r->phase && t->last.attempts == r->attempts && t->error == d.tracked_error) continue;
        cJSON *j = envelope(t->session, t->request_id); char suffix[64];
        cJSON_AddStringToObject(j, "stage", "delivery"); cJSON_AddStringToObject(j, "phase", rf_control_phase_name(r->phase));
        cJSON_AddNumberToObject(j, "attempts", r->attempts); cJSON_AddNumberToObject(j, "error", d.tracked_error);
        cJSON_AddBoolToObject(j, "pending", rf_control_active(r));
        cJSON_AddNumberToObject(j, "requested_centi_c", r->centi_c); cJSON_AddNumberToObject(j, "requested_mode", r->mode);
        if (r->phase == RF_CONTROL_MATCHED) cJSON_AddNumberToObject(j, "match_age_ms", now()-r->matched_ms);
        snprintf(suffix, sizeof(suffix), "radiators/%u/result", (unsigned)t->id);
        if (json_publish(suffix, j)) { t->last = *r; t->error = d.tracked_error; t->published = true; }
    }
}
static void observations(void) {
    radiator_record d; int64_t time = now();
    cJSON *bridge = envelope(session, 0);
    cJSON_AddNumberToObject(bridge, "uptime_ms", (double)time);
    cJSON_AddNumberToObject(bridge, "request_id_floor", (double)high_water);
    cJSON_AddNumberToObject(bridge, "stale_after_ms", RF_REPORT_STALE_MS);
    json_publish("bridge", bridge);
    for (size_t i = 0; radio_device_at(i, &d) == ESP_OK; ++i) {
        cJSON *j = envelope(session, 0); char suffix[64];
        cJSON_AddNumberToObject(j, "id", d.id); cJSON_AddStringToObject(j, "name", d.name);
        cJSON_AddBoolToObject(j, "available", d.has_paired_report);
        if (d.has_paired_report) {
            int64_t age = time-d.paired_report_time_us/1000;
            cJSON_AddNumberToObject(j, "age_ms", (double)age);
            cJSON_AddBoolToObject(j, "stale", age >= RF_REPORT_STALE_MS);
            cJSON_AddNumberToObject(j, "mode", d.paired_report.mode);
            if (d.paired_report.temperature_available) cJSON_AddNumberToObject(j, "temperature_centi_c", d.paired_report.temperature_centi_c);
            if (d.paired_report.target_available) cJSON_AddNumberToObject(j, "target_centi_c", d.paired_report.target_centi_c);
        }
        snprintf(suffix, sizeof(suffix), "radiators/%u/state", (unsigned)d.id); json_publish(suffix, j);
    }
}
static void stop(void) {
    if (running) client.stop(availability);
    running = false; mqtt_bridge_connection(false); session[0] = 0;
}
void mqtt_bridge_tick(void) {
    mqtt_settings change; bool action;
    mqtt_platform_lock(); action = action_pending;
    if (action) { change = candidate; memset(&candidate, 0, sizeof(candidate)); action_pending = false; }
    mqtt_platform_unlock();
    if (action) {
        esp_err_t err = mqtt_store_save(&change);
        if (err == ESP_OK) stop();
        mqtt_platform_lock();
        status.config_error = err; status.pending = false;
        status.persistence_uncertain = err != ESP_OK;
        if (err == ESP_OK) { settings = change; update_settings_status(); }
        mqtt_platform_unlock(); memset(&change, 0, sizeof(change)); retry_ms = now();
    }
    network_status network; bool wifi = network_get_status(&network) == ESP_OK && network.state == NET_CONNECTED;
    if (!settings.configured || !settings.enabled || !wifi) { if (running) stop(); return; }
    if (!running && now() >= retry_ms) {
        char id[32]; snprintf(id, sizeof(id), "cleverbox-%s", status.bridge_id);
        esp_err_t err = client.start(&settings, id, availability);
        mqtt_platform_lock(); status.client_error = err; mqtt_platform_unlock();
        running = err == ESP_OK; retry_ms = now()+5000;
    }
    bool connected; unsigned epoch;
    mqtt_platform_lock(); connected = link_connected; epoch = link_epoch; mqtt_platform_unlock();
    bool reconnected = epoch != owner_epoch;
    if (reconnected) {
        owner_epoch = epoch; session[0] = 0; high_water = 0;
        if (connected) {
            char topic[MQTT_TOPIC_BYTES]; snprintf(topic, sizeof(topic), "%s/radiators/+/set", base);
            if (!client.subscribe(topic)) { stop(); retry_ms = now()+5000; return; }
            client.nonce(session);
            mqtt_platform_lock();
            // A disconnect during subscribe must not revive connected status.
            if (epoch == link_epoch && link_connected) { status.connected = true; strcpy(status.session, session); }
            else connected = false;
            mqtt_platform_unlock();
            if (connected) { client.publish(availability, "online", true); state_ms = 0; }
        }
    }
    if (!connected || !session[0]) return;
    results(reconnected);
    if (now() >= state_ms) { observations(); state_ms = now()+5000; }
    // Drain a bounded batch. Epoch checks discard work queued before link loss;
    // there is no offline input queue. Already-admitted RF runs independently.
    for (size_t count = 0; count < MQTT_INPUT_SLOTS; ++count) {
        input in; bool got;
        mqtt_platform_lock(); got = input_count != 0;
        if (got) { in = inputs[input_head]; input_head = (input_head+1)%MQTT_INPUT_SLOTS; --input_count; }
        bool current = got && in.epoch == link_epoch && link_connected;
        mqtt_platform_unlock();
        if (!got) break;
        if (current && in.epoch == owner_epoch) route(&in);
    }
}
