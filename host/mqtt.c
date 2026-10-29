// SPDX-License-Identifier: MIT
// Deterministic fake-client I/O, not a second routing/publication implementation.
#include "mqtt_bridge.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static bool started, connected;

static char publications[64][1024];
static size_t head, count;
void mqtt_platform_lock(void) {}
void mqtt_platform_unlock(void) {}
static esp_err_t start(const mqtt_settings *s, const char *id, const char *will) {
    (void)s; (void)id; (void)will; started = true; return ESP_OK;
}
static bool publish(const char *topic, const char *payload, bool retain) {
    if (!connected) return false;
    cJSON *j = cJSON_CreateObject(); cJSON_AddStringToObject(j, "topic", topic);
    cJSON_AddStringToObject(j, "payload", payload); cJSON_AddBoolToObject(j, "retain", retain);
    size_t index = (head+count)%64;
    if (count == 64) head = (head+1)%64; else ++count;
    cJSON_PrintPreallocated(j, publications[index], sizeof(publications[index]), false); cJSON_Delete(j); return true;
}
static void stop(const char *will) { if (connected) publish(will, "offline", true); started = connected = false; }
static bool subscribe(const char *topic) { (void)topic; return connected; }
static void nonce(char out[33]) {
    unsigned char b[16]; FILE *f = fopen("/dev/urandom", "rb");
    if (!f || fread(b, 1, sizeof(b), f) != sizeof(b)) abort();
    fclose(f);
    for (size_t i = 0; i < sizeof(b); ++i) snprintf(out+i*2, 3, "%02x", b[i]);
}
esp_err_t mqtt_bridge_start(void) {
    mqtt_client_ops ops = {start, stop, subscribe, publish, nonce}; mqtt_bridge_init(ops, "020000000001"); return ESP_OK;
}
int host_mqtt_connection(int value) {
    if (!started) return 0;
    connected = value != 0; mqtt_bridge_connection(connected); return 1;
}
const char *host_mqtt_pop(void) {
    if (!count) return "";
    const char *p = publications[head]; head = (head+1)%64; --count; return p;
}
