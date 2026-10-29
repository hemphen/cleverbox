// SPDX-License-Identifier: MIT
#include "device_time.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <sys/time.h>
static SemaphoreHandle_t mutex;
void time_platform_lock(void) { xSemaphoreTake(mutex, portMAX_DELAY); }
void time_platform_unlock(void) { xSemaphoreGive(mutex); }
esp_err_t time_platform_set_utc(int64_t seconds) {
    struct timeval tv = {.tv_sec = (time_t)seconds};
    return settimeofday(&tv, NULL) == 0 ? ESP_OK : ESP_FAIL;
}
static void synchronized(struct timeval *tv) { device_time_synchronized(tv->tv_sec); }
static void network_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)data;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
        device_time_network(true, esp_netif_sntp_start()); // Also restarts after reconnect.
    else if ((base == WIFI_EVENT && (id == WIFI_EVENT_STA_DISCONNECTED || id == WIFI_EVENT_STA_STOP)) ||
             (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP)) device_time_network(false, ESP_OK);
}
esp_err_t device_time_start(void) {
    mutex = xSemaphoreCreateMutex(); if (!mutex) return ESP_ERR_NO_MEM;
    device_time_init();
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    config.start = false; config.wait_for_sync = false; config.smooth_sync = false;
    config.sync_cb = synchronized;
    esp_err_t err = esp_netif_sntp_init(&config);
    device_time_network(false, err);
    // SNTP failure leaves explicit manual time and HTTP/radio available.
    if (err != ESP_OK) return ESP_OK;
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, network_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, network_event, NULL));
    return ESP_OK;
}
