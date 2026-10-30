// SPDX-License-Identifier: MIT
// Deterministic fake-client I/O, not a second routing/publication implementation.
#include "mqtt_bridge.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static bool started, connected;
static int subscription_id, last_subscription_id, starts, subscribe_mode;

static char publications[64][16384];
static size_t head, count;
// Socket-boundary model: an unacknowledged QoS1 packet belongs to the client,
// survives connection loss and may retransmit until that client is destroyed.
static char outbox_topic[MQTT_TOPIC_BYTES], outbox_payload[MQTT_DISCOVERY_PAYLOAD_BYTES];
static int outbox_mid, outbox_discarded, replay_attempts, replayed;
int host_mqtt_outbox_pending(void) { return outbox_mid != 0; }
int host_mqtt_outbox_discarded(void) { return outbox_discarded; }
int host_mqtt_outbox_replay_attempts(void) { return replay_attempts; }
int host_mqtt_outbox_replayed(void) { return replayed; }
void mqtt_platform_lock(void) {}
void mqtt_platform_unlock(void) {}
static esp_err_t start(const mqtt_settings *s, const char *id, const char *will) {
    (void)s; (void)id; (void)will; started = true; ++starts; return ESP_OK;
}
static bool publish(const char *topic, const char *payload, bool retain) {
    if (!connected) return false;
    cJSON *j = cJSON_CreateObject(); cJSON_AddStringToObject(j, "topic", topic);
    cJSON_AddStringToObject(j, "payload", payload); cJSON_AddBoolToObject(j, "retain", retain);
    size_t index = (head+count)%64;
    if (count == 64) head = (head+1)%64; else ++count;
    cJSON_PrintPreallocated(j, publications[index], sizeof(publications[index]), false); cJSON_Delete(j); return true;
}
static void stop(const char *will) {
    if (connected) publish(will, "offline", true);
    if (outbox_mid) ++outbox_discarded;
    outbox_mid = 0; started = connected = false;
}
int host_mqtt_replay_outbox(void) {
    ++replay_attempts;
    if (outbox_mid && connected && publish(outbox_topic, outbox_payload, true)) {
        ++replayed; mqtt_bridge_puback(outbox_mid); outbox_mid = 0;
    }
    return 1;
}
static int subscribe(const char *topic) {
    (void)topic; if (!connected || subscribe_mode == 2) return -1;
    last_subscription_id = ++subscription_id;
    if (subscribe_mode == 1) mqtt_bridge_suback(last_subscription_id, 0);
    return last_subscription_id;
}
int host_mqtt_subscribe_mode(int mode) {
    if (mode < 0 || mode > 2) return 0;
    subscribe_mode = mode; return 1;
}
int host_mqtt_subscription_id(void) { return last_subscription_id; }
int host_mqtt_starts(void) { return starts; }
static int confirmed_mode;
int host_mqtt_confirmed_mode(int mode) { confirmed_mode = mode; return 1; }
static int publish_confirmed(const char *topic, const char *payload) {
    if (confirmed_mode == 1 || !publish(topic, payload, true)) return -1;
    int id = ++subscription_id;
    if (confirmed_mode != 2) mqtt_bridge_puback(id);
    else if (!outbox_mid) {
        strcpy(outbox_topic, topic); strcpy(outbox_payload, payload); outbox_mid = id;
    }
    return id;
}
static void nonce(char out[33]) {
    unsigned char b[16]; FILE *f = fopen("/dev/urandom", "rb");
    if (!f || fread(b, 1, sizeof(b), f) != sizeof(b)) abort();
    fclose(f);
    for (size_t i = 0; i < sizeof(b); ++i) snprintf(out+i*2, 3, "%02x", b[i]);
}
esp_err_t mqtt_bridge_start(void) {
    mqtt_client_ops ops = {start, stop, subscribe, publish, nonce, publish_confirmed}; mqtt_bridge_init(ops, "020000000001"); return ESP_OK;
}
int host_mqtt_connection(int value) {
    if (!started) return 0;
    connected = value != 0; mqtt_bridge_connection(connected); return 1;
}
const char *host_mqtt_pop(void) {
    if (!count) return "";
    const char *p = publications[head]; head = (head+1)%64; --count; return p;
}
