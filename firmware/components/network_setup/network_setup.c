// SPDX-License-Identifier: MIT
// Wi-Fi setup policy, shared by the firmware and the host simulation.
#include "network_setup.h"
#include "device_store.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static SemaphoreHandle_t mutex;
static network_status status;
static network_credentials saved;
static int64_t next_attempt_ms, pressed_ms = -1;
static bool button_armed;

static void take(void) { xSemaphoreTake(mutex, portMAX_DELAY); }
static void give(void) { xSemaphoreGive(mutex); }

bool network_credentials_valid(const network_credentials *c)
{
    if (!c) return false;
    const char *s = memchr(c->ssid, 0, sizeof(c->ssid)), *p = memchr(c->password, 0, sizeof(c->password));
    if (!s || !p || s == c->ssid || p - c->password < 8) return false;
    for (const unsigned char *v = (const unsigned char *)c->password; *v; ++v)
        if (*v < 32 || *v > 126) return false;
    return true;
}

const char *network_state_name(network_state s)
{
    static const char *names[] = {"disconnected", "connecting", "connected"};
    return names[s];
}

static void apply(void)
{
    esp_err_t e = network_platform_apply(status.ap, status.configured ? &saved : NULL);
    if (e != ESP_OK) status.error = e;
    if (status.ap) ESP_LOGI("network", "Setup AP %s open at http://192.168.4.1/", NETWORK_AP_SSID);
}

static void connect(void)
{
    status.state = NET_CONNECTING;
    status.ip[0] = 0;
    esp_err_t e = network_platform_connect();
    if (e != ESP_OK) { status.error = e; status.state = NET_DISCONNECTED; }
    next_attempt_ms = esp_timer_get_time() / 1000 + NETWORK_RETRY_MS;
}

void network_init(void)
{
    mutex = xSemaphoreCreateMutex();
    size_t size = sizeof(saved);
    status.configured = store_load("wifi", &saved, &size) == ESP_OK && size == sizeof(saved) &&
                        network_credentials_valid(&saved);
    status.ap = !status.configured; // Unconfigured devices open the setup AP.
    apply();
}

void network_get_status(network_status *out)
{
    take(); *out = status; give();
}

esp_err_t network_configure(const network_credentials *c)
{
    if (!network_credentials_valid(c)) return ESP_ERR_INVALID_ARG;
    take();
    esp_err_t e = store_save("wifi", c, sizeof(*c));
    saved = *c;
    status.configured = true;
    status.error = e;
    apply();
    connect();
    give();
    return e;
}

esp_err_t network_forget(void)
{
    take();
    esp_err_t e = store_erase("wifi");
    memset(&saved, 0, sizeof(saved));
    status.configured = false;
    status.state = NET_DISCONNECTED;
    status.ip[0] = 0;
    status.ap = true;
    status.error = e;
    apply();
    give();
    return e;
}

esp_err_t network_close_ap(void)
{
    take();
    esp_err_t e = status.configured ? ESP_OK : ESP_ERR_INVALID_STATE;
    if (e == ESP_OK && status.ap) { status.ap = false; apply(); }
    give();
    return e;
}

void network_connected(const char *ip)
{
    take();
    status.state = NET_CONNECTED;
    status.error = ESP_OK;
    strncpy(status.ip, ip, sizeof(status.ip) - 1);
    give();
    ESP_LOGI("network", "Connected: http://%s/", ip);
}

void network_disconnected(void)
{
    take();
    if (status.state != NET_DISCONNECTED) next_attempt_ms = esp_timer_get_time() / 1000 + NETWORK_RETRY_MS;
    status.state = NET_DISCONNECTED;
    status.ip[0] = 0;
    give();
}

void network_tick(int64_t now, bool button_low)
{
    take();
    if (status.configured && status.state == NET_DISCONNECTED && now >= next_attempt_ms) connect();
    // BOOT held for five seconds, then released, opens the setup AP. A press
    // held since startup counts only after a release.
    if (!button_low) {
        if (pressed_ms >= 0 && now - pressed_ms >= NETWORK_BUTTON_HOLD_MS && !status.ap) {
            status.ap = true;
            apply();
        }
        button_armed = true;
        pressed_ms = -1;
    } else if (button_armed && pressed_ms < 0) {
        pressed_ms = now;
    }
    give();
}
