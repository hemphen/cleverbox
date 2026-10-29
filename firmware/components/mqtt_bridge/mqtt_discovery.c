// SPDX-License-Identifier: MIT
#include "mqtt_discovery.h"
#include "radio_transport.h"
#include "cJSON.h"
#include "nvs.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>

// Persist before publishing: even a power cut after broker acceptance leaves a
// known topic to replay/delete. Capacity bounds churn while offline; no ID range
// scans, wildcard deletions or unbounded tombstone list. No per-heartbeat writes.
#define SLOTS (2 * CLEVERBOX_RADIATOR_CAPACITY)
#define RECORD_BYTES (8 + 4 * SLOTS)
static uint32_t ids[SLOTS];
static bool enabled, valid, replay;
static char bridge[13];
static unsigned cursor;
static uint32_t generation;
static int error, pending_mid, ack_mid;
static uint32_t pending_id;
static bool pending_delete;
static int64_t due, deadline;
static char payload[MQTT_DISCOVERY_PAYLOAD_BYTES]; // never on task stack
static unsigned char record[RECORD_BYTES];
static int64_t now(void) { return esp_timer_get_time()/1000; }
static esp_err_t save(void) {
    if (generation == UINT32_MAX) return ESP_ERR_INVALID_STATE;
    ++generation; // Different bytes force NVS to resolve same-value retry writes.
    memset(record, 0, sizeof(record)); record[0] = 1; record[1] = enabled;
    for (unsigned b = 0; b < 4; ++b) record[4+b] = generation >> (8*b);
    for (size_t i = 0; i < SLOTS; ++i)
        for (unsigned b = 0; b < 4; ++b) record[8+4*i+b] = ids[i] >> (8*b);
    nvs_handle_t h; esp_err_t err = nvs_open("cleverbox", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    // Never erase the journal before replacing it: a torn update must leave
    // an old/new record that still accounts for every advertised topic.
    if (err == ESP_OK) err = nvs_set_blob(h, "ha_discovery", record, sizeof(record));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    // Verify with a new handle; keep RAM journal and halt on any uncertainty.
    if (err == ESP_OK) {
        err = nvs_open("cleverbox", NVS_READONLY, &h);
        if (err == ESP_OK) {
            size_t size = sizeof(record);
            // Independent bounded verification buffer, never on the task stack.
            static unsigned char verify[RECORD_BYTES];
            err = nvs_get_blob(h, "ha_discovery", verify, &size); nvs_close(h);
            if (err == ESP_OK && (size != sizeof(record) || memcmp(record, verify, size))) err = ESP_FAIL;
        }
    }
    return err;
}
void mqtt_discovery_init(const char *identity) {
    strcpy(bridge, identity); memset(ids, 0, sizeof(ids)); enabled = false;
    generation = 0; valid = true; error = 0; cursor = 0; replay = true; pending_mid = ack_mid = 0; due = 0;
    nvs_handle_t h; size_t size = sizeof(record);
    esp_err_t err = nvs_open("cleverbox", NVS_READONLY, &h);
    if (err == ESP_OK) { err = nvs_get_blob(h, "ha_discovery", record, &size); nvs_close(h); }
    if (err == ESP_ERR_NVS_NOT_FOUND) return;
    if (err == ESP_OK && (size != sizeof(record) || record[0] != 1 || record[1] > 1 || record[2] || record[3])) err = ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) {
        enabled = record[1];
        for (unsigned b = 0; b < 4; ++b) generation |= (uint32_t)record[4+b] << (8*b);
        for (size_t i = 0; i < SLOTS; ++i) {
            for (unsigned b = 0; b < 4; ++b) ids[i] |= (uint32_t)record[8+4*i+b] << (8*b);
            for (size_t j = 0; ids[i] && j < i; ++j) if (ids[j] == ids[i]) err = ESP_ERR_INVALID_STATE;
        }
    }
    if (err != ESP_OK) { valid = false; enabled = false; error = err; }
}
bool mqtt_discovery_has_records(void) {
    if (!valid) return true; // Don't strand an unreadable cleanup journal on another broker.
    for (size_t i = 0; i < SLOTS; ++i) if (ids[i]) return true;
    return false;
}
esp_err_t mqtt_discovery_set(bool value) {
    if (!valid) return ESP_ERR_INVALID_STATE;
    bool old = enabled; enabled = value;
    esp_err_t err = save();
    if (err != ESP_OK) enabled = old;
    error = err; mqtt_discovery_replay(); return err;
}
void mqtt_discovery_status(mqtt_bridge_status *out) {
    out->discovery_enabled = enabled; out->discovery_error = error;
    unsigned count = pending_mid ? 1 : 0;
    radiator_record d;
    for (size_t i = 0; i < SLOTS; ++i)
        if (ids[i] && (!enabled || radio_device_status(ids[i], &d) != ESP_OK)) ++count;
    if (enabled) for (size_t i = 0; radio_device_at(i, &d) == ESP_OK; ++i) {
        bool found = false;
        for (size_t k = 0; k < SLOTS; ++k) if (ids[k] == d.id) found = true;
        if (!found) ++count;
    }
    out->discovery_pending = count;
}
void mqtt_discovery_replay(void) { replay = true; cursor = 0; due = 0; }
void mqtt_discovery_disconnected(void) {
    mqtt_platform_lock(); pending_mid = ack_mid = 0; mqtt_platform_unlock();
    if (error == MQTT_ERR_SUBSCRIBE_TIMEOUT) error = 0;
    mqtt_discovery_replay();
}
void mqtt_bridge_puback(int mid) {
    mqtt_platform_lock();
    if (pending_mid && (pending_mid == -1 || pending_mid == mid)) ack_mid = mid;
    mqtt_platform_unlock();
}
static bool build(const radiator_record *d) {
    char identity[64], topic[128], uid[96], entity[96];
    snprintf(identity, sizeof(identity), "cleverbox_%s_%u", bridge, (unsigned)d->id);
    snprintf(topic, sizeof(topic), "cleverbox/v1/%s/radiators/%u/state", bridge, (unsigned)d->id);
    cJSON *root = cJSON_CreateObject(), *dev = cJSON_AddObjectToObject(root, "device");
    cJSON_AddStringToObject(dev, "identifiers", identity);
    char fallback[48]; snprintf(fallback, sizeof(fallback), "Radiator %u", (unsigned)d->id);
    cJSON_AddStringToObject(dev, "name", d->name[0] ? d->name : fallback);
    cJSON_AddStringToObject(dev, "manufacturer", "Cleverbox");
    cJSON_AddStringToObject(dev, "model", "Radiator RF bridge endpoint");
    cJSON *origin = cJSON_AddObjectToObject(root, "origin");
    cJSON_AddStringToObject(origin, "name", "Cleverbox");
    cJSON_AddStringToObject(origin, "support_url", "https://github.com/hemphen/cleverbox");
    cJSON_AddStringToObject(root, "state_topic", topic);
    char avail[96]; snprintf(avail, sizeof(avail), "cleverbox/v1/%s/availability", bridge);
    cJSON *components = cJSON_AddObjectToObject(root, "components");
    static const char *keys[] = {"temperature_centi_c", "target_centi_c", "mode", "fresh", "age", "admission", "delivery"};
    static const char *names[] = {"Temperature", "Observed target", "Observed mode", "Observation fresh", "Report age", "Last admission", "Last delivery"};
    static const char *entity_keys[] = {"temperature", "observed_target", "observed_mode", "observation_fresh", "report_age", "last_admission", "last_delivery"};
    for (size_t i = 0; i < 7; ++i) {
        cJSON *c = cJSON_AddObjectToObject(components, keys[i]);
        cJSON_AddStringToObject(c, "platform", i == 3 ? "binary_sensor" : "sensor");
        snprintf(uid, sizeof(uid), "%s_%s", identity, keys[i]);
        snprintf(entity, sizeof(entity), "%s.cleverbox_%s_%u_%s", i == 3 ? "binary_sensor" : "sensor", bridge, (unsigned)d->id, entity_keys[i]);
        cJSON_AddStringToObject(c, "unique_id", uid);
        cJSON_AddStringToObject(c, "default_entity_id", entity);
        cJSON_AddStringToObject(c, "name", names[i]);
        cJSON_AddNumberToObject(c, "expire_after", 15);
        cJSON_AddStringToObject(c, "availability_mode", "all");
        cJSON *a = cJSON_AddArrayToObject(c, "availability"), *v = cJSON_CreateObject();
        cJSON_AddItemToArray(a, v); cJSON_AddStringToObject(v, "topic", avail);
        v = cJSON_CreateObject(); cJSON_AddItemToArray(a, v);
        cJSON_AddStringToObject(v, "topic", topic);
        char templ[320];
        if (i < 3) snprintf(templ, sizeof(templ), "{{ 'online' if value_json.fresh and value_json.%s is defined else 'offline' }}", keys[i]);
        else strcpy(templ, "online");
        cJSON_AddStringToObject(v, "value_template", templ);
        if (i < 2) {
            cJSON_AddStringToObject(c, "device_class", "temperature");
            cJSON_AddStringToObject(c, "unit_of_measurement", "°C");
            if (!i) cJSON_AddStringToObject(c, "state_class", "measurement");
            snprintf(templ, sizeof(templ), "{{ value_json.%s | float / 100 if value_json.%s is defined else none }}", keys[i], keys[i]);
        } else if (i == 2) strcpy(templ, "{{ {0:'Comfort',1:'Off',2:'Frost',3:'Eco'}.get(value_json.mode, 'Unknown') if value_json.mode is defined else 'Unknown' }}");
        else if (i == 3) {
            strcpy(templ, "{{ 'ON' if value_json.fresh else 'OFF' }}");
            cJSON_AddStringToObject(c, "entity_category", "diagnostic");
        } else {
            cJSON_AddStringToObject(c, "entity_category", "diagnostic");
            if (i == 4) { strcpy(templ, "{{ value_json.age_ms / 1000 if value_json.age_ms is defined else none }}"); cJSON_AddStringToObject(c, "unit_of_measurement", "s"); cJSON_AddStringToObject(c, "device_class", "duration"); }
            else {
                snprintf(templ, sizeof(templ), "{{ value_json.%s.%s | default('none') }}", keys[i], i == 5 ? "reason" : "phase");
                cJSON_AddStringToObject(c, "json_attributes_topic", topic);
                char attrs[160]; snprintf(attrs, sizeof(attrs), "{{ dict(result=value_json.%s | default({})) | to_json }}", keys[i]);
                cJSON_AddStringToObject(c, "json_attributes_template", attrs);
            }
        }
        cJSON_AddStringToObject(c, "value_template", templ);
    }
    bool ok = cJSON_PrintPreallocated(root, payload, sizeof(payload), false); cJSON_Delete(root); return ok;
}
void mqtt_discovery_tick(mqtt_client_ops *client) {
    if (!valid || error || now() < due) return;
    mqtt_platform_lock(); int mid = pending_mid, ack = ack_mid; mqtt_platform_unlock();
    if (mid) {
        if (ack == mid) {
            if (pending_delete) {
                for (size_t i = 0; i < SLOTS; ++i) if (ids[i] == pending_id) {
                    ids[i] = 0; error = save();
                    if (error) ids[i] = pending_id;
                    break;
                }
            }
            mqtt_platform_lock(); pending_mid = ack_mid = 0; mqtt_platform_unlock();
            due = now()+100; return;
        }
        if (now() >= deadline) {
            // Owner closes the client to discard a possibly stuck QoS1 outbox.
            error = MQTT_ERR_SUBSCRIBE_TIMEOUT;
        }
        return;
    }
    // Register new topics durably before sending any configuration.
    if (enabled) {
        radiator_record d;
        for (size_t i = 0; radio_device_at(i, &d) == ESP_OK; ++i) {
            bool found = false; size_t empty = SLOTS;
            for (size_t k = 0; k < SLOTS; ++k) { if (ids[k] == d.id) found = true; if (!ids[k]) empty = k; }
            if (!found && empty < SLOTS) {
                ids[empty] = d.id; error = save();
                if (error) { ids[empty] = 0; return; }
                replay = true;
            }
            // Full journal leaves the new radiator undiscovered until cleanup.
        }
    }
    for (unsigned slot = cursor; slot < SLOTS; ++slot) {
        if (!ids[slot]) continue;
        radiator_record d; bool remove = !enabled || radio_device_status(ids[slot], &d) != ESP_OK;
        if (!remove && !replay) continue;
        if (!remove && !build(&d)) { error = ESP_ERR_INVALID_ARG; return; }
        char topic[128]; snprintf(topic, sizeof(topic), "homeassistant/device/cleverbox_%s_%u/config", bridge, (unsigned)ids[slot]);
        pending_id = ids[slot]; pending_delete = remove;
        mqtt_platform_lock(); pending_mid = -1; ack_mid = 0; mqtt_platform_unlock();
        int sent = client->publish_confirmed(topic, remove ? "" : payload);
        mqtt_platform_lock(); pending_mid = sent > 0 ? sent : 0; mqtt_platform_unlock();
        if (sent <= 0) { due = now()+MQTT_RETRY_MS; return; }
        deadline = now()+MQTT_SUBACK_TIMEOUT_MS;
        cursor = slot+1;
        // One bounded sweep. Registry/name changes and HA birth schedule replay;
        // periodic replay also repairs broker persistence loss without RF.
        return;
    }
    replay = true; cursor = 0; due = now()+5000;
}
