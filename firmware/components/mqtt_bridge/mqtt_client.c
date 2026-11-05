// SPDX-License-Identifier: MIT
// ESP-MQTT client for mqtt_bridge.c. ESP-MQTT reconnects by itself.
#include "mqtt_bridge.h"
#include "mqtt_client.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static esp_mqtt_client_handle_t handle;

static void event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    esp_mqtt_event_handle_t e = data;
    if (id == MQTT_EVENT_CONNECTED) mqtt_bridge_connected(true);
    else if (id == MQTT_EVENT_DISCONNECTED) mqtt_bridge_connected(false);
    else if (id == MQTT_EVENT_DATA && e->current_data_offset == 0 && e->data_len == e->total_data_len) {
        // Commands are small; fragmented messages are ignored.
        char topic[128];
        if (e->topic_len <= 0 || e->topic_len >= (int)sizeof(topic)) return;
        memcpy(topic, e->topic, e->topic_len);
        topic[e->topic_len] = 0;
        mqtt_bridge_message(topic, e->data, (size_t)e->data_len, e->retain);
    }
}

static esp_err_t start(const mqtt_settings *s, const char *id, const char *will)
{
    esp_mqtt_client_config_t config = {
        .broker.address = {.hostname = s->host, .transport = MQTT_TRANSPORT_OVER_TCP, .port = s->port},
        .credentials = {.username = s->username[0] ? s->username : NULL, .client_id = id,
                        .authentication.password = s->username[0] ? s->password : NULL},
        .session = {.last_will = {.topic = will, .msg = "offline", .qos = 1, .retain = true}, .keepalive = 30},
        .network = {.reconnect_timeout_ms = 5000},
        .buffer = {.size = 1024},
        .task.stack_size = 6144,
    };
    handle = esp_mqtt_client_init(&config);
    if (!handle) return ESP_ERR_NO_MEM;
    esp_err_t e = esp_mqtt_client_register_event(handle, ESP_EVENT_ANY_ID, event, NULL);
    if (e == ESP_OK) e = esp_mqtt_client_start(handle);
    if (e != ESP_OK) { esp_mqtt_client_destroy(handle); handle = NULL; }
    return e;
}

static void stop(void)
{
    if (!handle) return;
    esp_mqtt_client_stop(handle);
    esp_mqtt_client_destroy(handle);
    handle = NULL;
}

static bool publish(const char *topic, const char *payload, int qos, bool retain)
{
    return handle && esp_mqtt_client_publish(handle, topic, payload, 0, qos, retain) >= 0;
}

static bool subscribe(const char *topic) { return handle && esp_mqtt_client_subscribe(handle, topic, 0) >= 0; }

static void task(void *arg)
{
    (void)arg;
    for (;;) {
        mqtt_bridge_tick(esp_timer_get_time() / 1000);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

esp_err_t mqtt_bridge_start(void)
{
    uint8_t mac[6];
    esp_err_t e = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (e != ESP_OK) return e;
    char id[13];
    for (size_t i = 0; i < sizeof(mac); ++i) snprintf(id + i * 2, 3, "%02x", mac[i]);
    mqtt_client_ops ops = {start, stop, publish, subscribe};
    mqtt_bridge_init(ops, id);
    return xTaskCreate(task, "mqtt_bridge", 6144, NULL, 3, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
