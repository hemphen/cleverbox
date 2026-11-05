// SPDX-License-Identifier: MIT
// MQTT topics, commands, snapshots and Home Assistant discovery. See docs/mqtt.md.
#include "mqtt_bridge.h"
#include "radio_transport.h"
#include "device_store.h"
#include "device_time.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static mqtt_client_ops client;
static SemaphoreHandle_t mutex;
// Guarded by mutex: settings, status and the flags below.
static mqtt_settings settings;
static mqtt_bridge_status status;
static bool restart, replay;
static int64_t next_snapshot;
// Bridge task only.
static bool running;
static uint32_t published_version;
static uint32_t advertised[CLEVERBOX_RADIATOR_CAPACITY]; // Discovery configs on the broker.
static size_t advertised_count;
static radiator_record records[CLEVERBOX_RADIATOR_CAPACITY];
static char base[48], availability[64];

static void take(void) { xSemaphoreTake(mutex, portMAX_DELAY); }
static void give(void) { xSemaphoreGive(mutex); }

static void update_status(void)
{
    status.configured = settings.host[0] != 0;
    status.enabled = settings.enabled; status.discovery = settings.discovery; status.port = settings.port;
    strcpy(status.host, settings.host); strcpy(status.username, settings.username);
}

void mqtt_bridge_init(mqtt_client_ops ops, const char bridge_id[13])
{
    client = ops;
    if (!mutex) mutex = xSemaphoreCreateMutex();
    memset(&status, 0, sizeof(status));
    strcpy(status.bridge_id, bridge_id);
    snprintf(base, sizeof(base), "cleverbox/v1/%s", bridge_id);
    snprintf(availability, sizeof(availability), "%s/availability", base);
    struct { uint8_t version; mqtt_settings s; } stored = {0};
    size_t size = sizeof(stored);
    if (store_load("mqtt", &stored, &size) == ESP_OK && size == sizeof(stored) && stored.version == 1 &&
        mqtt_settings_valid(&stored.s)) settings = stored.s;
    else memset(&settings, 0, sizeof(settings));
    update_status();
    restart = true;
    running = false; advertised_count = 0; published_version = 0;
}

void mqtt_bridge_get(mqtt_bridge_status *out) { take(); *out = status; give(); }

static bool printable(const char *s)
{
    for (; *s; ++s) if ((unsigned char)*s < 32 || (unsigned char)*s > 126) return false;
    return true;
}

bool mqtt_settings_valid(const mqtt_settings *s)
{
    if (!s->host[0]) return true; // Not configured.
    if (!s->port || !memchr(s->host, 0, sizeof(s->host)) || !memchr(s->username, 0, sizeof(s->username)) ||
        !memchr(s->password, 0, sizeof(s->password)) || !printable(s->username) || !printable(s->password)) return false;
    for (const char *p = s->host; *p; ++p)
        if (!isalnum((unsigned char)*p) && *p != '.' && *p != '-') return false; // Hostname or IPv4.
    return true;
}

// Discovery changes keep the connection; broker changes restart the client.
static esp_err_t save(const mqtt_settings *s, bool reconnect)
{
    struct { uint8_t version; mqtt_settings s; } stored = {1, *s};
    esp_err_t e = s->host[0] ? store_save("mqtt", &stored, sizeof(stored)) : store_erase("mqtt");
    take();
    settings = *s; update_status(); status.error = e;
    restart |= reconnect;
    replay = true;
    give();
    return e;
}

esp_err_t mqtt_bridge_configure(const mqtt_settings *s)
{
    if (!s->host[0] || !mqtt_settings_valid(s)) return ESP_ERR_INVALID_ARG;
    take(); mqtt_settings next = *s; next.discovery = settings.discovery; give();
    return save(&next, true);
}

esp_err_t mqtt_bridge_action(const char *action)
{
    take(); mqtt_settings next = settings; give();
    if (!action) return ESP_ERR_INVALID_ARG;
    bool discovery = !strncmp(action, "discovery_", 10);
    if (!strcmp(action, "forget")) memset(&next, 0, sizeof(next));
    else if (!strcmp(action, "discovery_enable")) next.discovery = true;
    else if (!strcmp(action, "discovery_disable")) next.discovery = false;
    else if ((!strcmp(action, "enable") || !strcmp(action, "disable")) && next.host[0]) next.enabled = !strcmp(action, "enable");
    else return ESP_ERR_INVALID_ARG;
    return save(&next, !discovery);
}

static void publish_json(const char *suffix, cJSON *j, int qos, bool retain)
{
    char topic[96], *payload = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    snprintf(topic, sizeof(topic), "%s/%s", base, suffix);
    if (payload) client.publish(topic, payload, qos, retain);
    cJSON_free(payload);
}

static void address_text(const uint8_t a[4], char out[9])
{
    snprintf(out, 9, "%02X%02X%02X%02X", a[0], a[1], a[2], a[3]);
}

void mqtt_bridge_connected(bool connected)
{
    if (connected) {
        char topic[96];
        snprintf(topic, sizeof(topic), "%s/radiators/+/set", base);
        client.subscribe(topic);
        client.subscribe("homeassistant/status");
        client.publish(availability, "online", 1, true);
    }
    take();
    status.connected = connected;
    if (connected) { replay = true; next_snapshot = 0; }
    give();
}

static bool integer(cJSON *j, const char *key, double low, double high, double *out)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(j, key);
    if (!cJSON_IsNumber(v) || v->valuedouble < low || v->valuedouble > high ||
        floor(v->valuedouble) != v->valuedouble) return false;
    *out = v->valuedouble;
    return true;
}

// Command: {"mode":0..3,"centi_c":0..3000[,"request_id":N]} on B/radiators/<id>/set.
void mqtt_bridge_message(const char *topic, const char *payload, size_t length, bool retained)
{
    if (!strcmp(topic, "homeassistant/status")) {
        if (length == 6 && !memcmp(payload, "online", 6)) { take(); replay = true; next_snapshot = 0; give(); }
        return;
    }
    size_t n = strlen(base);
    char *end = NULL;
    unsigned long id = 0;
    if (!strncmp(topic, base, n) && !strncmp(topic + n, "/radiators/", 11) && isdigit((unsigned char)topic[n + 11]))
        id = strtoul(topic + n + 11, &end, 10);
    cJSON *result = cJSON_CreateObject();
    if (!id || id > UINT32_MAX || strcmp(end, "/set")) {
        cJSON_AddBoolToObject(result, "accepted", false);
        cJSON_AddStringToObject(result, "reason", "invalid_topic");
        cJSON_AddStringToObject(result, "topic", topic);
        publish_json("errors", result, 0, false);
        return;
    }
    cJSON *j = length <= MQTT_PAYLOAD_BYTES ? cJSON_ParseWithLength(payload, length) : NULL;
    double mode, centi_c, request_id = 0;
    bool id_ok = !cJSON_HasObjectItem(j, "request_id") || integer(j, "request_id", 1, 9007199254740991.0, &request_id);
    const char *reason;
    // A retained command would be replayed at every reconnect and boot.
    if (retained) reason = "retained_command";
    else if (!id_ok || !integer(j, "mode", 0, 3, &mode) || !integer(j, "centi_c", 0, 3000, &centi_c)) reason = "invalid_request";
    else {
        esp_err_t e = radio_command((uint32_t)id, (rf_mode)mode, (int)centi_c, (uint64_t)request_id);
        reason = e == ESP_OK ? "accepted" : radio_error_name(e);
    }
    cJSON_Delete(j);
    if (request_id) cJSON_AddNumberToObject(result, "request_id", request_id);
    cJSON_AddBoolToObject(result, "accepted", !strcmp(reason, "accepted"));
    cJSON_AddStringToObject(result, "reason", reason);
    char suffix[48];
    snprintf(suffix, sizeof(suffix), "radiators/%lu/result", id);
    publish_json(suffix, result, 0, false);
}

static const char *phase_name(const radiator_record *d)
{
    return d->command_queued ? "queued" : rf_control_phase_name(d->command.phase);
}

static void publish_snapshots(int64_t now, size_t count)
{
    radio_status radio = radio_transport_status();
    device_time_status time;
    device_time_get(&time);
    cJSON *bridge = cJSON_CreateObject();
    cJSON_AddNumberToObject(bridge, "uptime_ms", (double)now);
    cJSON_AddBoolToObject(bridge, "radio_ready", radio.ready);
    cJSON_AddStringToObject(bridge, "radio_error_stage", radio.error_stage);
    cJSON_AddNumberToObject(bridge, "radio_error", radio.error);
    cJSON_AddBoolToObject(bridge, "time_usable", time.usable);
    publish_json("bridge", bridge, 0, false);
    for (size_t i = 0; i < count; ++i) {
        const radiator_record *d = &records[i];
        cJSON *j = cJSON_CreateObject();
        char text[9], suffix[48];
        cJSON_AddNumberToObject(j, "id", d->id);
        cJSON_AddStringToObject(j, "name", d->name);
        address_text(d->radiator, text); cJSON_AddStringToObject(j, "radiator", text);
        cJSON_AddBoolToObject(j, "paired", radiator_paired(d));
        cJSON_AddBoolToObject(j, "radio_ready", radio.ready);
        cJSON_AddBoolToObject(j, "available", d->has_report);
        int64_t age = now - d->report_ms;
        // 15 s headroom: HA expires snapshots after 15 s, so a value published
        // as fresh is never shown past the five-minute stale threshold.
        cJSON_AddBoolToObject(j, "fresh", radio.ready && d->has_report && age + 15000 < RF_REPORT_STALE_MS);
        if (d->has_report) {
            cJSON_AddNumberToObject(j, "age_ms", (double)age);
            cJSON_AddBoolToObject(j, "stale", age >= RF_REPORT_STALE_MS);
            address_text(d->report.destination, text); cJSON_AddStringToObject(j, "controller", text);
            cJSON_AddNumberToObject(j, "mode", d->report.mode);
            if (d->report.temperature_available) cJSON_AddNumberToObject(j, "temperature_centi_c", d->report.temperature_centi_c);
            if (d->report.target_available) cJSON_AddNumberToObject(j, "target_centi_c", d->report.target_centi_c);
        }
        if (radiator_paired(d)) {
            cJSON *c = cJSON_AddObjectToObject(j, "command");
            cJSON_AddStringToObject(c, "phase", phase_name(d));
            cJSON_AddNumberToObject(c, "attempts", d->command.attempts);
            cJSON_AddNumberToObject(c, "error", d->command_error);
            if (d->command_queued || d->command.phase != RF_CONTROL_IDLE) {
                cJSON_AddNumberToObject(c, "requested_mode", d->command.mode);
                cJSON_AddNumberToObject(c, "requested_centi_c", d->command.centi_c);
            }
            if (d->request_id) cJSON_AddNumberToObject(c, "request_id", (double)d->request_id);
        }
        snprintf(suffix, sizeof(suffix), "radiators/%u/state", (unsigned)d->id);
        publish_json(suffix, j, 0, false);
    }
}

static void config_topic(uint32_t id, char *out, size_t size)
{
    snprintf(out, size, "homeassistant/device/cleverbox_%s_%u/config", status.bridge_id, (unsigned)id);
}

// One device per radiator. Readings are unavailable unless fresh and present.
static void publish_config(const radiator_record *d)
{
    static const struct { const char *key, *platform, *name, *field, *value, *unit, *device_class; bool diagnostic, paired; } parts[] = {
        {"temperature", "sensor", "Temperature", "temperature_centi_c", "{{ value_json.temperature_centi_c / 100 }}", "°C", "temperature", false, false},
        {"observed_target", "sensor", "Observed target", "target_centi_c", "{{ value_json.target_centi_c / 100 }}", "°C", "temperature", false, false},
        {"observed_mode", "sensor", "Observed mode", "mode", "{{ {0:'Comfort',1:'Off',2:'Frost',3:'Eco'}.get(value_json.mode, 'Unknown') }}", NULL, NULL, false, false},
        {"observation_fresh", "binary_sensor", "Observation fresh", NULL, "{{ 'ON' if value_json.fresh else 'OFF' }}", NULL, NULL, true, false},
        {"report_age", "sensor", "Report age", NULL, "{{ value_json.age_ms / 1000 if value_json.age_ms is defined else none }}", "s", "duration", true, false},
        {"last_command", "sensor", "Last command", NULL, "{{ value_json.command.phase }}", NULL, NULL, true, true},
        {"last_admission", "sensor", "Last admission", NULL, "{{ value_json.reason }}", NULL, NULL, true, true},
    };
    char identity[48], state[96], result[96], text[160], topic[112];
    snprintf(identity, sizeof(identity), "cleverbox_%s_%u", status.bridge_id, (unsigned)d->id);
    snprintf(state, sizeof(state), "%s/radiators/%u/state", base, (unsigned)d->id);
    snprintf(result, sizeof(result), "%s/radiators/%u/result", base, (unsigned)d->id);
    cJSON *root = cJSON_CreateObject(), *device = cJSON_AddObjectToObject(root, "device");
    cJSON_AddStringToObject(device, "identifiers", identity);
    cJSON_AddStringToObject(device, "name", d->name);
    cJSON_AddStringToObject(device, "manufacturer", "Cleverbox");
    cJSON_AddStringToObject(device, "model", radiator_paired(d) ? "Paired radiator" : "Monitored radiator");
    cJSON_AddStringToObject(cJSON_AddObjectToObject(root, "origin"), "name", "Cleverbox");
    cJSON *components = cJSON_AddObjectToObject(root, "components");
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); ++i) {
        if (parts[i].paired && !radiator_paired(d)) continue;
        bool admission = !strcmp(parts[i].key, "last_admission");
        cJSON *c = cJSON_AddObjectToObject(components, parts[i].key);
        cJSON_AddStringToObject(c, "platform", parts[i].platform);
        snprintf(text, sizeof(text), "%s_%s", identity, parts[i].key);
        cJSON_AddStringToObject(c, "unique_id", text);
        snprintf(text, sizeof(text), "%s.%s_%s", parts[i].platform, identity, parts[i].key);
        cJSON_AddStringToObject(c, "default_entity_id", text);
        cJSON_AddStringToObject(c, "name", parts[i].name);
        cJSON_AddStringToObject(c, "state_topic", admission ? result : state);
        cJSON_AddStringToObject(c, "value_template", parts[i].value);
        if (parts[i].unit) cJSON_AddStringToObject(c, "unit_of_measurement", parts[i].unit);
        if (parts[i].device_class) cJSON_AddStringToObject(c, "device_class", parts[i].device_class);
        if (parts[i].diagnostic) cJSON_AddStringToObject(c, "entity_category", "diagnostic");
        if (!admission) cJSON_AddNumberToObject(c, "expire_after", 15);
        cJSON *a = cJSON_AddArrayToObject(c, "availability"), *v = cJSON_CreateObject();
        cJSON_AddStringToObject(v, "topic", availability);
        cJSON_AddItemToArray(a, v);
        if (parts[i].field) {
            v = cJSON_CreateObject();
            cJSON_AddStringToObject(v, "topic", state);
            snprintf(text, sizeof(text), "{{ 'online' if value_json.fresh and value_json.%s is defined else 'offline' }}", parts[i].field);
            cJSON_AddStringToObject(v, "value_template", text);
            cJSON_AddItemToArray(a, v);
            cJSON_AddStringToObject(c, "availability_mode", "all");
        }
    }
    config_topic(d->id, topic, sizeof(topic));
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload) client.publish(topic, payload, 1, true);
    cJSON_free(payload);
}

// Publish configs after registry changes, on (re)connect and on HA birth.
// An empty retained config removes a device from HA and from the broker.
static void sync_discovery(bool enabled, bool force, size_t count)
{
    uint32_t version = radio_transport_status().registry_version;
    char topic[112];
    if (enabled && (force || version != published_version)) {
        for (size_t i = 0; i < advertised_count; ++i) {
            bool present = false;
            for (size_t k = 0; k < count; ++k) present |= records[k].id == advertised[i];
            if (!present) { config_topic(advertised[i], topic, sizeof(topic)); client.publish(topic, "", 1, true); }
        }
        for (size_t i = 0; i < count; ++i) { publish_config(&records[i]); advertised[i] = records[i].id; }
        advertised_count = count;
        published_version = version;
    } else if (!enabled && advertised_count) {
        for (size_t i = 0; i < advertised_count; ++i) {
            config_topic(advertised[i], topic, sizeof(topic)); client.publish(topic, "", 1, true);
        }
        advertised_count = 0;
    }
}

void mqtt_bridge_tick(int64_t now)
{
    take();
    bool start = restart, force = replay, connected = status.connected, snapshot = now >= next_snapshot;
    mqtt_settings s = settings;
    restart = false;
    if (connected) replay = false;
    if (connected && snapshot) next_snapshot = now + MQTT_SNAPSHOT_MS;
    give();
    if (start) {
        if (running) {
            if (connected) client.publish(availability, "offline", 0, true); // A clean stop sends no will.
            client.stop();
        }
        running = false;
        take(); status.connected = false; give();
        if (s.enabled && s.host[0]) {
            char id[32];
            snprintf(id, sizeof(id), "cleverbox-%s", status.bridge_id);
            esp_err_t e = client.start(&s, id, availability);
            running = e == ESP_OK;
            take(); status.error = e; give();
        }
        return;
    }
    if (!connected) return;
    size_t count = radio_radiators(records, CLEVERBOX_RADIATOR_CAPACITY);
    sync_discovery(s.discovery, force, count);
    if (snapshot) publish_snapshots(now, count);
}
