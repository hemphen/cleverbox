// SPDX-License-Identifier: MIT
// ESP-IDF Wi-Fi adapter for network_setup.c.
#include "network_setup.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static bool started;

esp_err_t network_platform_apply(bool ap, const network_credentials *station)
{
    esp_err_t e = esp_wifi_set_mode(ap ? (station ? WIFI_MODE_APSTA : WIFI_MODE_AP) : WIFI_MODE_STA);
    if (e == ESP_OK && station) {
        wifi_config_t current = {0}, wanted = {0};
        memcpy(wanted.sta.ssid, station->ssid, strlen(station->ssid));
        memcpy(wanted.sta.password, station->password, strlen(station->password));
        wanted.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        esp_wifi_get_config(WIFI_IF_STA, &current);
        if (memcmp(current.sta.ssid, wanted.sta.ssid, sizeof(wanted.sta.ssid)) ||
            memcmp(current.sta.password, wanted.sta.password, sizeof(wanted.sta.password))) {
            esp_wifi_disconnect();
            e = esp_wifi_set_config(WIFI_IF_STA, &wanted);
        }
    }
    if (e == ESP_OK && !started) {
        e = esp_wifi_start();
        started = e == ESP_OK;
    }
    return e;
}

esp_err_t network_platform_connect(void) { return esp_wifi_connect(); }

static void event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) network_disconnected();
    else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) network_disconnected();
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        char ip[16];
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&((ip_event_got_ip_t *)data)->ip_info.ip));
        network_connected(ip);
    }
}

static void task(void *arg)
{
    (void)arg;
    for (;;) {
        network_tick(esp_timer_get_time() / 1000, gpio_get_level(NETWORK_BUTTON_GPIO) == 0);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

esp_err_t network_setup_start(void)
{
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    init.nvs_enable = false; // Credentials live in the application's "wifi" record.
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, event, NULL));
    // BOOT/GPIO9 is active low with a pull-up. It is a strapping pin: do not
    // hold it during reset.
    gpio_config_t button = {.pin_bit_mask = 1ULL << NETWORK_BUTTON_GPIO, .mode = GPIO_MODE_INPUT,
                            .pull_up_en = GPIO_PULLUP_ENABLE};
    ESP_ERROR_CHECK(gpio_config(&button));
    wifi_config_t ap = {.ap = {.ssid = NETWORK_AP_SSID, .password = NETWORK_AP_PASSWORD, .channel = 1,
                               .max_connection = 2, .authmode = WIFI_AUTH_WPA2_PSK}};
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    network_init();
    return xTaskCreate(task, "network", 3072, NULL, 3, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
