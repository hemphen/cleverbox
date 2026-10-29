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
#define MQTT_TOPIC_BYTES 128
#define MQTT_INPUT_SLOTS 4
#define MQTT_SUBACK_TIMEOUT_MS 5000
#define MQTT_RETRY_MS 5000
#define MQTT_ERR_SUBSCRIBE_REJECTED 0x7401
#define MQTT_ERR_SUBSCRIBE_TIMEOUT 0x7402
#define MQTT_ERR_SUBSCRIBE_REQUEST 0x7403
#define MQTT_RECORD_BYTES (4 + MQTT_HOST_BYTES+1 + MQTT_USER_BYTES+1 + MQTT_PASSWORD_BYTES+1)
typedef struct {
    bool configured, enabled;
    uint16_t port;
    char host[MQTT_HOST_BYTES+1], username[MQTT_USER_BYTES+1], password[MQTT_PASSWORD_BYTES+1];
} mqtt_settings;
typedef struct {
    bool configured, enabled, connected, pending, persistence_uncertain;
    bool broker_connected, subscription_pending;
    char bridge_id[13], session[33], host[MQTT_HOST_BYTES+1], username[MQTT_USER_BYTES+1];
    uint16_t port;
    int config_error, client_error, subscription_error;
    uint32_t dropped_input;
} mqtt_bridge_status;
// Injectable client boundary. Only the MQTT owner calls these (never RF/callbacks).
// publish is QoS0, online-only, no outbox. stop sends graceful offline if possible.
typedef struct {
    esp_err_t (*start)(const mqtt_settings *, const char *client_id, const char *will_topic);
    void (*stop)(const char *will_topic);
    int (*subscribe)(const char *topic); // MQTT packet ID (1..65535), negative on failure.
    bool (*publish)(const char *topic, const char *payload, bool retain);
    void (*nonce)(char out[33]);
} mqtt_client_ops;
esp_err_t mqtt_bridge_start(void);
void mqtt_bridge_get(mqtt_bridge_status *out);
esp_err_t mqtt_bridge_configure(const mqtt_settings *settings);
esp_err_t mqtt_bridge_action(const char *action); // enable, disable, forget
bool mqtt_json_flat(const char *payload);
bool mqtt_settings_valid(const mqtt_settings *settings);
esp_err_t mqtt_store_load(mqtt_settings *out);
esp_err_t mqtt_store_save(const mqtt_settings *settings);
// One owner initializes/ticks the service. Callbacks only copy bounded input.
void mqtt_bridge_init(mqtt_client_ops ops, const char bridge_id[13]);
void mqtt_bridge_tick(void);
void mqtt_bridge_connection(bool connected);
void mqtt_bridge_client_error(esp_err_t error);
// Current client only: grant must be QoS 0 for our single QoS-0 subscription.
void mqtt_bridge_suback(int message_id, int granted_qos);
void mqtt_bridge_input(const char *topic, size_t topic_len, const char *data,
                       size_t length, size_t offset, size_t total, bool retained);
void mqtt_platform_lock(void);
void mqtt_platform_unlock(void);
#ifdef __cplusplus
}
#endif
