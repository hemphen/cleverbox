// SPDX-License-Identifier: MIT
#include "network_setup.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

typedef enum { STARTED, DISCONNECTED, GOT_IP, CONFIGURE, FORGET } event_kind;
typedef struct { event_kind kind; network_credentials credentials; char ip[16]; } message;
static QueueHandle_t events;
static SemaphoreHandle_t mutex;
static network_status published;
static bool pending;
static atomic_bool overflow;
static network_policy policy;
static bool ap_enabled = true;
static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
static esp_err_t set_ap(bool enabled) {
    wifi_mode_t mode;
    esp_err_t err = esp_wifi_get_mode(&mode);
    if (err == ESP_OK) err = esp_wifi_set_mode(enabled ?
        (mode == WIFI_MODE_AP ? WIFI_MODE_AP : WIFI_MODE_APSTA) : WIFI_MODE_STA);
    if (err == ESP_OK) {
        ap_enabled = enabled;
        if (enabled) ESP_LOGI("network", "Recovery AP Cleverbox available at http://192.168.4.1 (password printed at boot)");
    }
    return err;
}
static esp_err_t start_attempt(const network_credentials *c) {
    // Keep AP running. Disable/re-enable only STA, creating a STA_START event
    // barrier after old disconnect/IP events. Never stop/restart the Wi-Fi driver.
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    wifi_config_t config = {0};
    memcpy(config.sta.ssid, c->ssid, strlen(c->ssid));
    memcpy(config.sta.password, c->password, strlen(c->password));
    config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_STA, &config);
    memset(&config, 0, sizeof(config)); return err;
}
static esp_err_t associate(void) { return esp_wifi_connect(); }
static void stop_station(void) {
    // Also invalidate queued events in the policy until the next STA_START.
    if (esp_wifi_set_mode(WIFI_MODE_AP) == ESP_OK) ap_enabled = true;
}
static void event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; message m = {0};
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) m.kind = STARTED;
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) m.kind = DISCONNECTED;
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        m.kind = GOT_IP; ip_event_got_ip_t *ip = data;
        snprintf(m.ip, sizeof(m.ip), IPSTR, IP2STR(&ip->ip_info.ip));
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) m.kind = DISCONNECTED;
    else return;
    if (xQueueSend(events, &m, 0) != pdTRUE) atomic_store(&overflow, true);
}
static void publish(bool release) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    // Read actual mode tracked by the adapter even after a platform failure.
    policy.status.recovery_ap = ap_enabled;
    published = policy.status;
    if (release) pending = false;
    xSemaphoreGive(mutex);
}
static void run(void *arg) {
    (void)arg; network_credentials saved = {0};
    esp_err_t err = network_store_load(&saved);
    network_platform platform = {set_ap, start_attempt, associate, stop_station,
                                 network_store_save, network_store_forget};
    network_policy_init(&policy, platform, err == ESP_OK ? &saved : NULL,
                        err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND, now_ms());
    memset(&saved, 0, sizeof(saved)); publish(true);
    for (;;) {
        message m = {0}; bool release = false;
        if (xQueueReceive(events, &m, pdMS_TO_TICKS(250)) == pdTRUE) {
            int64_t now = now_ms();
            switch (m.kind) {
                case STARTED: network_policy_started(&policy, now); break;
                case DISCONNECTED: network_policy_disconnected(&policy, now); break;
                case GOT_IP:
                    network_policy_got_ip(&policy, m.ip, now);
                    if (policy.status.state == NET_CONNECTED)
                        ESP_LOGI("network", "Application at http://%s", policy.status.ip);
                    break;
                case CONFIGURE: network_policy_configure(&policy, &m.credentials, now); break;
                case FORGET: network_policy_forget(&policy, now); break;
            }
            release = m.kind == CONFIGURE || m.kind == FORGET;
            memset(&m, 0, sizeof(m));
        }
        if (atomic_exchange(&overflow, false)) {
            // Lost events cannot strand setup; force the usual bounded recovery.
            if (policy.attempting) network_policy_tick(&policy, policy.deadline_ms);
            else network_policy_disconnected(&policy, now_ms());
        }
        network_policy_tick(&policy, now_ms()); publish(release);
    }
}
static esp_err_t submit(message *m) {
    if (!mutex || xSemaphoreTake(mutex, pdMS_TO_TICKS(10)) != pdTRUE) return ESP_ERR_INVALID_STATE;
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (!pending && (m->kind == FORGET ||
        (published.state != NET_CONNECTING && published.setup != NET_SETUP_PENDING))) {
        if (xQueueSend(events, m, 0) == pdTRUE) { pending = true; err = ESP_OK; }
    }
    xSemaphoreGive(mutex); return err;
}
esp_err_t network_configure(const network_credentials *c) {
    if (!network_credentials_valid(c)) return ESP_ERR_INVALID_ARG;
    message m = {.kind = CONFIGURE, .credentials = *c};
    esp_err_t err = submit(&m); memset(&m, 0, sizeof(m)); return err;
}
esp_err_t network_forget(void) { message m = {.kind = FORGET}; return submit(&m); }
esp_err_t network_get_status(network_status *out) {
    if (!mutex || xSemaphoreTake(mutex, pdMS_TO_TICKS(10)) != pdTRUE) return ESP_ERR_INVALID_STATE;
    *out = published; xSemaphoreGive(mutex); return ESP_OK;
}
esp_err_t network_setup_start(void) {
    events = xQueueCreate(16, sizeof(message)); mutex = xSemaphoreCreateMutex();
    if (!events || !mutex) return ESP_ERR_NO_MEM;
    published = (network_status){.state = NET_DISCONNECTED, .recovery_ap = true, .error = "none"};
    pending = true; // Do not admit requests until saved configuration has loaded.
    esp_netif_create_default_wifi_ap(); esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    // Suppress Wi-Fi driver's connection diagnostics (which can include SSIDs).
    esp_log_level_set("wifi", ESP_LOG_NONE);
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, event, NULL));
    wifi_config_t config = {.ap = {.ssid = "Cleverbox", .channel = 1,
                                   .max_connection = 2, .authmode = WIFI_AUTH_WPA2_PSK}};
    snprintf((char *)config.ap.password, sizeof(config.ap.password), "%08lx%08lx",
             (unsigned long)esp_random(), (unsigned long)esp_random());
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &config));
    ESP_ERROR_CHECK(esp_wifi_start());
    // Recovery credential only. Never log the station SSID/password.
    ESP_LOGI("network", "Recovery Wi-Fi Cleverbox password: %s; http://192.168.4.1", config.ap.password);
    memset(&config, 0, sizeof(config));
    if (xTaskCreate(run, "network_setup", 4096, NULL, 3, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}
