// SPDX-License-Identifier: MIT
#include "mqtt_bridge.h"
#include "mqtt_client.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static esp_mqtt_client_handle_t handle;
void mqtt_platform_lock(void) { portENTER_CRITICAL(&lock); }
void mqtt_platform_unlock(void) { portEXIT_CRITICAL(&lock); }
static void event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base; esp_mqtt_event_handle_t e = data;
    if (id == MQTT_EVENT_CONNECTED) mqtt_bridge_connection(true);
    else if (id == MQTT_EVENT_DISCONNECTED) mqtt_bridge_connection(false);
    else if (id == MQTT_EVENT_SUBSCRIBED) {
        // IDF 5.5.1 deliver_suback exposes the grant bytes in data, including 0x80
        // rejection, and marks MQTT_ERROR_TYPE_SUBSCRIBE_FAILED on failure.
        bool granted = e->data && e->data_len == 1 && (unsigned char)e->data[0] == 0 &&
            (!e->error_handle || e->error_handle->error_type != MQTT_ERROR_TYPE_SUBSCRIBE_FAILED);
        mqtt_bridge_suback(e->msg_id, granted ? 0 : -1);
    }
    else if (id == MQTT_EVENT_PUBLISHED) mqtt_bridge_puback(e->msg_id);
    else if (id == MQTT_EVENT_ERROR) mqtt_bridge_client_error(ESP_FAIL);
    else if (id == MQTT_EVENT_DATA && e->topic_len >= 0 && e->data_len >= 0 &&
             e->current_data_offset >= 0 && e->total_data_len >= 0)
        mqtt_bridge_input(e->topic, (size_t)e->topic_len, e->data, (size_t)e->data_len,
                          (size_t)e->current_data_offset, (size_t)e->total_data_len, e->retain);
}
static esp_err_t start(const mqtt_settings *s, const char *id, const char *will) {
    esp_mqtt_client_config_t config = {
        .broker.address = {.hostname = s->host, .transport = MQTT_TRANSPORT_OVER_TCP, .port = s->port},
        .credentials = {.username = s->username[0] ? s->username : NULL, .client_id = id,
                        .authentication.password = s->username[0] ? s->password : NULL},
        .session = {.last_will = {.topic = will, .msg = "offline", .qos = 1, .retain = true},
                    .disable_clean_session = false, .keepalive = 30, .protocol_ver = MQTT_PROTOCOL_V_3_1_1},
        .network = {.reconnect_timeout_ms = 5000, .timeout_ms = 1000},
        .buffer = {.size = 256, .out_size = 1024},
        .outbox.limit = MQTT_DISCOVERY_PAYLOAD_BYTES + 1024,
        .task.stack_size = 6144,
    };
    handle = esp_mqtt_client_init(&config);
    if (!handle) return ESP_ERR_NO_MEM;
    esp_err_t err = esp_mqtt_client_register_event(handle, ESP_EVENT_ANY_ID, event, NULL);
    if (err == ESP_OK) err = esp_mqtt_client_start(handle);
    if (err != ESP_OK) { esp_mqtt_client_destroy(handle); handle = NULL; }
    return err;
}
static void stop(const char *will) {
    if (!handle) return;
    // A failed offline write must leave the broker's will armed. Never send a
    // graceful DISCONNECT after a failed publication that would suppress it.
    if (esp_mqtt_client_publish(handle, will, "offline", 0, 0, true) >= 0)
        esp_mqtt_client_disconnect(handle);
    esp_mqtt_client_stop(handle); esp_mqtt_client_destroy(handle); handle = NULL;
}
static int subscribe(const char *topic) { return handle ? esp_mqtt_client_subscribe(handle, topic, 0) : -1; }
static bool publish(const char *topic, const char *data, bool retain) {
    // Called only while connected. QoS0 never enters the IDF retransmit outbox.
    return handle && esp_mqtt_client_publish(handle, topic, data, 0, 0, retain) >= 0;
}
static int publish_confirmed(const char *topic, const char *data) {
    return handle ? esp_mqtt_client_publish(handle, topic, data, 0, 1, true) : -1;
}
static void nonce(char out[33]) {
    uint8_t bytes[16]; esp_fill_random(bytes, sizeof(bytes));
    for (size_t i = 0; i < sizeof(bytes); ++i) snprintf(out+i*2, 3, "%02x", bytes[i]);
}
static void task(void *arg) { (void)arg; for (;;) { mqtt_bridge_tick(); vTaskDelay(pdMS_TO_TICKS(100)); } }
esp_err_t mqtt_bridge_start(void) {
    // Do not print broker payloads/credentials from the client on auth errors.
    esp_log_level_set("mqtt_client", ESP_LOG_NONE);
    uint8_t mac[6]; esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA); if (err != ESP_OK) return err;
    char id[13]; for (size_t i = 0; i < sizeof(mac); ++i) snprintf(id+i*2, 3, "%02x", mac[i]);
    mqtt_client_ops ops = {start, stop, subscribe, publish, nonce, publish_confirmed}; mqtt_bridge_init(ops, id);
    return xTaskCreate(task, "mqtt_bridge", 8192, NULL, 3, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
