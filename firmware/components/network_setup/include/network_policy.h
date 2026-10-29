// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
// WPA2 personal networks only, byte lengths excluding NUL. No credentials in status.
typedef struct { char ssid[33]; char password[64]; } network_credentials;
typedef enum { NET_DISCONNECTED, NET_CONNECTING, NET_CONNECTED, NET_FAILED } network_state;
typedef enum { NET_SETUP_NONE, NET_SETUP_PENDING, NET_SETUP_SAVED, NET_SETUP_FAILED } network_setup_result;
typedef struct {
    network_state state;
    network_setup_result setup;
    bool configured, recovery_ap;
    unsigned attempts;
    char ip[16];
    const char *error;
} network_status;
// Called by one owner only. start resets STA and delivers a fresh STA_START
// barrier before associate; old disconnect/IP events during reset are ignored.
typedef struct {
    esp_err_t (*ap)(bool enabled);
    esp_err_t (*start)(const network_credentials *);
    esp_err_t (*associate)(void);
    void (*stop)(void);
    esp_err_t (*save)(const network_credentials *);
    esp_err_t (*forget)(void);
} network_platform;
typedef struct {
    network_status status;
    network_credentials saved, candidate;
    network_platform platform;
    bool candidate_active, ready, attempting;
    int64_t deadline_ms, next_ms, close_ap_ms;
} network_policy;
#define NETWORK_ATTEMPT_MS 15000
#define NETWORK_RETRY_MS 5000
#define NETWORK_RECOVERY_RETRY_MS 60000
#define NETWORK_AP_GRACE_MS 60000
#define NETWORK_MAX_ATTEMPTS 3
bool network_credentials_valid(const network_credentials *);
void network_policy_init(network_policy *, network_platform, const network_credentials *, bool load_failed, int64_t now);
void network_policy_configure(network_policy *, const network_credentials *, int64_t now);
void network_policy_forget(network_policy *, int64_t now);
void network_policy_tick(network_policy *, int64_t now);
void network_policy_started(network_policy *, int64_t now);
void network_policy_disconnected(network_policy *, int64_t now);
void network_policy_got_ip(network_policy *, const char *ip, int64_t now);
const char *network_state_name(network_state);
const char *network_setup_name(network_setup_result);
#ifdef __cplusplus
}
#endif
