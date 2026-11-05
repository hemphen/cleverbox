// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
#define MQTT_HOST_BYTES 96
#define MQTT_USER_BYTES 64
#define MQTT_PASSWORD_BYTES 128
#define MQTT_PAYLOAD_BYTES 512
#define MQTT_SNAPSHOT_MS 5000

typedef struct {
    bool enabled, discovery;
    uint16_t port;
    char host[MQTT_HOST_BYTES + 1], username[MQTT_USER_BYTES + 1], password[MQTT_PASSWORD_BYTES + 1];
} mqtt_settings;
typedef struct {
    bool configured, enabled, discovery, connected;
    uint16_t port;
    char host[MQTT_HOST_BYTES + 1], username[MQTT_USER_BYTES + 1], bridge_id[13];
    esp_err_t error; // Last settings save or client start error.
} mqtt_bridge_status;

// Client boundary, implemented with ESP-MQTT on the device and by the host
// simulation. The client reconnects by itself and reports through
// mqtt_bridge_connected() and mqtt_bridge_message().
typedef struct {
    esp_err_t (*start)(const mqtt_settings *settings, const char *client_id, const char *will_topic);
    void (*stop)(void);
    bool (*publish)(const char *topic, const char *payload, int qos, bool retain);
    bool (*subscribe)(const char *topic);
} mqtt_client_ops;

// Platform start: creates the bridge task.
esp_err_t mqtt_bridge_start(void);
void mqtt_bridge_init(mqtt_client_ops ops, const char bridge_id[13]);
// Call about every 100 ms from one task: client start/stop, discovery, snapshots.
void mqtt_bridge_tick(int64_t now_ms);
void mqtt_bridge_connected(bool connected);
void mqtt_bridge_message(const char *topic, const char *payload, size_t length, bool retained);

void mqtt_bridge_get(mqtt_bridge_status *out);
bool mqtt_settings_valid(const mqtt_settings *settings);
// Replace broker settings; keeps the discovery choice.
esp_err_t mqtt_bridge_configure(const mqtt_settings *settings);
// "enable", "disable", "forget", "discovery_enable" or "discovery_disable".
esp_err_t mqtt_bridge_action(const char *action);
#ifdef __cplusplus
}
#endif
