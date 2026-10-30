// SPDX-License-Identifier: MIT
// Fake Wi-Fi platform/events only. Production policy and NVS format are reused.
#include "network_setup.h"
#include "network_button.h"
#include "device_time.h"
#include "esp_timer.h"
#include "nvs.h"
#include <string.h>
static network_policy policy;
static network_button button;
static bool button_low;
static esp_err_t ap(bool enabled) { (void)enabled; return ESP_OK; }
static esp_err_t start(const network_credentials *c) { (void)c; return ESP_OK; }
static esp_err_t associate(void) { return ESP_OK; }
static void stop(void) {}
esp_err_t network_setup_start(void) {
    network_credentials saved = {0}; esp_err_t err = network_store_load(&saved);
    network_platform platform = {ap, start, associate, stop, network_store_save, network_store_forget};
    network_policy_init(&policy, platform, err == ESP_OK ? &saved : NULL,
                        err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND, esp_timer_get_time() / 1000);
    memset(&saved, 0, sizeof(saved)); return ESP_OK;
}
esp_err_t network_get_status(network_status *out) { *out = policy.status; return ESP_OK; }
esp_err_t network_configure(const network_credentials *c) {
    if (!network_credentials_valid(c)) return ESP_ERR_INVALID_ARG;
    if (policy.attempting || policy.candidate_active) return ESP_ERR_INVALID_STATE;
    network_policy_configure(&policy, c, esp_timer_get_time() / 1000); return ESP_OK;
}
esp_err_t network_forget(void) { network_policy_forget(&policy, esp_timer_get_time() / 1000); return ESP_OK; }
esp_err_t network_finish_recovery(void) { network_policy_finish_recovery(&policy, esp_timer_get_time() / 1000); return ESP_OK; }
void host_network_button(int low) { button_low = low != 0; }
void host_network_tick(void) {
    if (network_button_sample(&button, button_low, esp_timer_get_time() / 1000))
        network_policy_recover(&policy, esp_timer_get_time() / 1000);
    network_policy_tick(&policy, esp_timer_get_time() / 1000);
    device_time_network(policy.status.state == NET_CONNECTED, ESP_OK);
}
int host_network_event(const char *name) {
    int64_t now = esp_timer_get_time() / 1000;
    if (!strcmp(name, "started")) network_policy_started(&policy, now);
    else if (!strcmp(name, "disconnected") || !strcmp(name, "lost_ip")) network_policy_disconnected(&policy, now);
    else if (!strcmp(name, "got_ip")) network_policy_got_ip(&policy, "192.0.2.10", now);
    else return 0;
    return 1;
}
