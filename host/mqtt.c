// SPDX-License-Identifier: MIT
// Fake MQTT client: records publications for tests. host/mqtt_client.py
// replaces it with a real broker connection (serve.py --real-mqtt).
#include "mqtt_bridge.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>

static bool started, connected;
static int starts;
static char *publications[256];
static size_t head, count;

static esp_err_t start(const mqtt_settings *s, const char *id, const char *will)
{
    (void)s; (void)id; (void)will; started = true; ++starts;
    return ESP_OK;
}
static void stop(void) { started = connected = false; }
static bool publish(const char *topic, const char *payload, int qos, bool retain)
{
    if (!connected) return false;
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "topic", topic);
    cJSON_AddStringToObject(j, "payload", payload);
    cJSON_AddNumberToObject(j, "qos", qos);
    cJSON_AddBoolToObject(j, "retain", retain);
    size_t index = (head + count) % 256;
    if (count == 256) { free(publications[head]); head = (head + 1) % 256; } else ++count;
    publications[index] = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    return true;
}
static bool subscribe(const char *topic) { (void)topic; return connected; }

esp_err_t mqtt_bridge_start(void)
{
    mqtt_client_ops ops = {start, stop, publish, subscribe};
    mqtt_bridge_init(ops, "020000000001");
    return ESP_OK;
}
int host_mqtt_connection(int value)
{
    if (!started) return 0;
    connected = value != 0;
    mqtt_bridge_connected(connected);
    return 1;
}
int host_mqtt_starts(void) { return starts; }
// Returns the oldest unread publication as JSON, or "" when none remain.
const char *host_mqtt_pop(void)
{
    static char *last;
    free(last); last = NULL;
    if (!count) return "";
    last = publications[head]; head = (head + 1) % 256; --count;
    return last;
}
