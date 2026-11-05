// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
// Fixed, published setup access point credentials.
#define NETWORK_AP_SSID "Cleverbox"
#define NETWORK_AP_PASSWORD "cleverbox"
#define NETWORK_BUTTON_GPIO 9
#define NETWORK_RETRY_MS 5000
#define NETWORK_BUTTON_HOLD_MS 5000

// WPA2 personal: SSID 1-32 bytes, password 8-63 printable ASCII characters.
typedef struct { char ssid[33]; char password[64]; } network_credentials;
typedef enum { NET_DISCONNECTED, NET_CONNECTING, NET_CONNECTED } network_state;
typedef struct {
    network_state state;
    bool configured, ap;
    char ip[16];
    esp_err_t error; // Last platform or storage error.
} network_status;

esp_err_t network_setup_start(void);
void network_get_status(network_status *out);
const char *network_state_name(network_state state);
bool network_credentials_valid(const network_credentials *c);
// Save credentials and connect. The setup AP stays as it is.
esp_err_t network_configure(const network_credentials *c);
// Erase credentials, disconnect and open the setup AP.
esp_err_t network_forget(void);
// Close the setup AP (a configured station keeps running).
esp_err_t network_close_ap(void);

// Portable core, called by the platform adapter (ESP or host simulation).
void network_init(void);
void network_connected(const char *ip);
void network_disconnected(void);
// Call about every 100 ms: reconnects and samples the BOOT button.
void network_tick(int64_t now_ms, bool button_low);
// Platform: set AP on/off and the station configuration (NULL = none).
esp_err_t network_platform_apply(bool ap, const network_credentials *station);
esp_err_t network_platform_connect(void);
#ifdef __cplusplus
}
#endif
