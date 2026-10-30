// SPDX-License-Identifier: MIT
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "esp_err.h"
#define ESP_ERR_NVS_NOT_FOUND 101
typedef void *QueueHandle_t;
typedef void *SemaphoreHandle_t;
typedef int esp_event_base_t;
enum { WIFI_EVENT = 1, IP_EVENT, WIFI_EVENT_STA_START, WIFI_EVENT_STA_DISCONNECTED,
       IP_EVENT_STA_GOT_IP, IP_EVENT_STA_LOST_IP, ESP_EVENT_ANY_ID, WIFI_IF_AP, WIFI_IF_STA,
       WIFI_AUTH_WPA2_PSK, WIFI_STORAGE_RAM, ESP_LOG_NONE, GPIO_MODE_INPUT,
       GPIO_PULLUP_ENABLE, GPIO_PULLDOWN_DISABLE, GPIO_INTR_DISABLE };
typedef enum { WIFI_MODE_AP, WIFI_MODE_STA, WIFI_MODE_APSTA } wifi_mode_t;
typedef struct { uint8_t a,b,c,d; } stub_ip;
typedef struct { struct { stub_ip ip; } ip_info; } ip_event_got_ip_t;
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(p) (p)->a,(p)->b,(p)->c,(p)->d
#define ESP_LOGI(...) ((void)0)
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(n) (n)
#define portMAX_DELAY (-1)
typedef struct { int unused; } wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){0})
typedef union {
    struct { uint8_t ssid[32],password[64]; int channel,max_connection,authmode; } ap;
    struct { uint8_t ssid[32],password[64]; struct { int authmode; } threshold; } sta;
} wifi_config_t;
typedef struct { uint64_t pin_bit_mask; int mode,pull_up_en,pull_down_en,intr_type; } gpio_config_t;
static int64_t clock_ms;
static int gpio_level = 1, mode_calls, associate_calls, config_calls;
static bool mutex_ok = true;
static wifi_mode_t wifi_mode;
static wifi_config_t ap_config;
static gpio_config_t button_config_seen;
static esp_err_t mode_error;
static size_t qcount, qsize, qcapacity;
static unsigned char queue_bytes[16][128];
static inline QueueHandle_t xQueueCreate(int n, size_t size) {
    assert(n <= 16 && size <= 128); qcount=0; qsize=size; qcapacity=(size_t)n; return queue_bytes;
}
static inline int xQueueSend(QueueHandle_t q, const void *m, int ticks) {
    (void)q; assert(ticks==0); if(qcount==qcapacity) return 0;
    memcpy(queue_bytes[qcount++],m,qsize); return pdTRUE;
}
static inline int xQueueReceive(QueueHandle_t q, void *m, int ticks) {
    (void)q; assert(ticks==20); if(!qcount) return 0;
    memcpy(m,queue_bytes[0],qsize); --qcount;
    memmove(queue_bytes[0],queue_bytes[1],qcount*sizeof(queue_bytes[0])); return pdTRUE;
}
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &mutex_ok; }
static inline int xSemaphoreTake(SemaphoreHandle_t m, int ticks) { (void)m;(void)ticks;return mutex_ok; }
static inline void xSemaphoreGive(SemaphoreHandle_t m) { (void)m; }
static inline int xTaskCreate(void (*fn)(void *), const char *name, int stack, void *arg, int priority, void *handle) {
    (void)fn;(void)name;(void)stack;(void)arg;(void)priority;(void)handle;return pdPASS;
}
static inline int64_t esp_timer_get_time(void) { return clock_ms*1000; }
static inline int gpio_get_level(int pin) { assert(pin==9);return gpio_level; }
static inline esp_err_t gpio_config(const gpio_config_t *c) { button_config_seen=*c;return ESP_OK; }
static inline esp_err_t esp_wifi_get_mode(wifi_mode_t *m) { *m=wifi_mode;return ESP_OK; }
static inline esp_err_t esp_wifi_set_mode(wifi_mode_t m) { ++mode_calls;if(!mode_error)wifi_mode=m;return mode_error; }
static inline esp_err_t esp_wifi_connect(void) { ++associate_calls;return ESP_OK; }
static inline esp_err_t esp_wifi_set_config(int iface, const wifi_config_t *c) { ++config_calls;if(iface==WIFI_IF_AP)ap_config=*c;return ESP_OK; }
static inline esp_err_t esp_wifi_init(const wifi_init_config_t *c) { (void)c;return ESP_OK; }
static inline esp_err_t esp_wifi_set_storage(int storage) { assert(storage==WIFI_STORAGE_RAM);return ESP_OK; }
static inline esp_err_t esp_wifi_start(void) { return ESP_OK; }
static inline void esp_netif_create_default_wifi_ap(void) {}
static inline void esp_netif_create_default_wifi_sta(void) {}
static inline void esp_log_level_set(const char *name, int level) { (void)name;(void)level; }
static inline esp_err_t esp_event_handler_register(esp_event_base_t base, int id,
    void (*fn)(void *,esp_event_base_t,int32_t,void *),void *arg) { (void)base;(void)id;(void)fn;(void)arg;return ESP_OK; }
