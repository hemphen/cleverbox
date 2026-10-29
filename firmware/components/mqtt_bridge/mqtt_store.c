// SPDX-License-Identifier: MIT
#include "mqtt_bridge.h"
#include "nvs.h"
#include <string.h>
#include <ctype.h>
static bool printable(const char *s, size_t max) {
    size_t n = 0;
    while (n <= max && s[n]) { if ((unsigned char)s[n] < 32 || (unsigned char)s[n] > 126) return false; ++n; }
    return n <= max;
}
bool mqtt_settings_valid(const mqtt_settings *s) {
    if (!s) return false;
    if (!s->configured) return !s->enabled && !s->port && !s->host[0] && !s->username[0] && !s->password[0];
    if (!s->port || !s->host[0] || !printable(s->host, MQTT_HOST_BYTES) ||
        !printable(s->username, MQTT_USER_BYTES) || !printable(s->password, MQTT_PASSWORD_BYTES) ||
        (s->password[0] && !s->username[0])) return false;
    // Explicit hostname/IPv4 (no URI, path, embedded credentials, IPv6 or TLS).
    for (const char *p = s->host; *p; ++p)
        if (!isalnum((unsigned char)*p) && *p != '.' && *p != '-') return false;
    return true;
}
static void encode(const mqtt_settings *s, unsigned char *b) {
    memset(b, 0, MQTT_RECORD_BYTES); b[0] = 1; b[1] = s->configured | (s->enabled << 1);
    b[2] = s->port; b[3] = s->port >> 8;
    memcpy(b+4, s->host, strlen(s->host));
    memcpy(b+4+MQTT_HOST_BYTES+1, s->username, strlen(s->username));
    memcpy(b+4+MQTT_HOST_BYTES+1+MQTT_USER_BYTES+1, s->password, strlen(s->password));
}
esp_err_t mqtt_store_load(mqtt_settings *out) {
    unsigned char b[MQTT_RECORD_BYTES]; size_t n = sizeof(b); nvs_handle_t h;
    esp_err_t err = nvs_open("cleverbox", NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    err = nvs_get_blob(h, "mqtt", b, &n); nvs_close(h);
    if (err != ESP_OK) return err;
    if (n != sizeof(b) || b[0] != 1 || b[1] > 3) return ESP_ERR_INVALID_STATE;
    mqtt_settings s = {0}; s.configured = b[1] & 1; s.enabled = b[1] & 2; s.port = b[2] | (b[3]<<8);
    memcpy(s.host, b+4, sizeof(s.host));
    memcpy(s.username, b+4+sizeof(s.host), sizeof(s.username));
    memcpy(s.password, b+4+sizeof(s.host)+sizeof(s.username), sizeof(s.password));
    if (!memchr(s.host, 0, sizeof(s.host)) || !memchr(s.username, 0, sizeof(s.username)) ||
        !memchr(s.password, 0, sizeof(s.password)) || !mqtt_settings_valid(&s)) return ESP_ERR_INVALID_STATE;
    unsigned char canonical[MQTT_RECORD_BYTES]; encode(&s, canonical);
    if (memcmp(b, canonical, sizeof(b))) return ESP_ERR_INVALID_STATE;
    *out = s; return ESP_OK;
}
esp_err_t mqtt_store_save(const mqtt_settings *s) {
    if (!mqtt_settings_valid(s)) return ESP_ERR_INVALID_ARG;
    unsigned char b[MQTT_RECORD_BYTES]; encode(s, b); nvs_handle_t h;
    esp_err_t err = nvs_open("cleverbox", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    // Forced erase/rewrite resolves same-value cached/deferred writes on retry.
    // Forget is a disabled tombstone, also force-written and verified, not cached absence.
    err = nvs_erase_key(h, "mqtt");
    if (err == ESP_OK) err = nvs_commit(h);
    else if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_set_blob(h, "mqtt", b, sizeof(b));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h); memset(b, 0, sizeof(b));
    if (err == ESP_OK) {
        mqtt_settings verified = {0}; err = mqtt_store_load(&verified);
        unsigned char actual[MQTT_RECORD_BYTES], expected[MQTT_RECORD_BYTES];
        if (err == ESP_OK) {
            encode(s, expected); encode(&verified, actual);
            if (memcmp(actual, expected, sizeof(actual))) err = ESP_FAIL;
            memset(actual, 0, sizeof(actual)); memset(expected, 0, sizeof(expected));
        }
        memset(&verified, 0, sizeof(verified));
    }
    return err;
}
