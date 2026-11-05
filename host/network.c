// SPDX-License-Identifier: MIT
// Fake Wi-Fi driver. network_setup.c runs unchanged; tests inject events.
#include "network_setup.h"
#include "esp_timer.h"
#include <string.h>

static bool button_low;
static int applies, connects;

esp_err_t network_platform_apply(bool ap, const network_credentials *station)
{
    (void)ap; (void)station; ++applies;
    return ESP_OK;
}
esp_err_t network_platform_connect(void) { ++connects; return ESP_OK; }
esp_err_t network_setup_start(void) { network_init(); return ESP_OK; }

void host_network_button(int low) { button_low = low != 0; }
void host_network_tick(void) { network_tick(esp_timer_get_time() / 1000, button_low); }
int host_network_connects(void) { return connects; }
int host_network_event(const char *name)
{
    if (!strcmp(name, "got_ip")) network_connected("192.0.2.10");
    else if (!strcmp(name, "disconnected")) network_disconnected();
    else return 0;
    return 1;
}
