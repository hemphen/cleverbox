// SPDX-License-Identifier: MIT
#include "device_time.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include <sys/time.h>

static void synchronized(struct timeval *tv) { device_time_synchronized(tv->tv_sec); }

// Start (or restart) SNTP whenever the station gets an address.
static void got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id; (void)data;
    esp_netif_sntp_start();
}

esp_err_t device_time_start(void)
{
    device_time_init();
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    config.start = false;
    config.sync_cb = synchronized;
    esp_err_t e = esp_netif_sntp_init(&config);
    if (e == ESP_OK) e = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, got_ip, NULL);
    if (e != ESP_OK) ESP_LOGE("time", "SNTP unavailable (%d); set time manually", e);
    return ESP_OK;
}
