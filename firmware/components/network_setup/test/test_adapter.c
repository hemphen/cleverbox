// SPDX-License-Identifier: MIT
// Exercise the actual ESP adapter loop and bounded admission at fake GPIO/RTOS/Wi-Fi boundaries.
#include "adapter_stubs.h"
#include "../network_setup.c"
static unsigned writes;
static esp_err_t load_error = ESP_FAIL;
esp_err_t network_store_load(network_credentials *c) { (void)c; return load_error; }
esp_err_t network_store_save(const network_credentials *c) { (void)c; ++writes; return ESP_OK; }
esp_err_t network_store_forget(void) { ++writes; return ESP_OK; }
static void at(int64_t now, int level) { clock_ms=now;gpio_level=level;step(); }
static void fill(void) { message m={.kind=STARTED};while(qcount<qcapacity) assert(xQueueSend(events,&m,0)); }
int main(void) {
    assert(network_setup_start()==ESP_OK);
    assert(wifi_mode == WIFI_MODE_NULL && !ap_enabled && !driver_started);
    assert(!strcmp((char *)ap_config.ap.ssid,"Cleverbox"));
    assert(!strcmp((char *)ap_config.ap.password,"cleverbox"));
    assert(ap_config.ap.authmode==WIFI_AUTH_WPA2_PSK);
    assert(button_config_seen.pin_bit_mask==(1ULL<<9));
    assert(button_config_seen.pull_up_en==GPIO_PULLUP_ENABLE && button_config_seen.intr_type==GPIO_INTR_DISABLE);
    assert(network_finish_recovery()==ESP_ERR_INVALID_STATE); // Startup load gate.
    load_policy();
    assert(!pending && !published.recovery_ap && !driver_started && !writes);
    assert(!strcmp(published.error,"storage_invalid"));
    network_platform platform={set_ap,start_attempt,associate,stop_station,network_store_save,network_store_forget};
    network_credentials c = {.ssid="generated", .password="testonly123"};
    network_policy_init(&policy,platform,&c,false,0); publish(true);
    assert(wifi_mode==WIFI_MODE_STA && !ap_enabled && driver_started);
    // Actual ESP adapter paths must never transiently choose AP/APSTA for retries.
    network_policy_started(&policy,0);
    event(NULL,IP_EVENT,IP_EVENT_STA_LOST_IP,NULL);step();
    assert(wifi_mode==WIFI_MODE_NULL && !ap_enabled);
    int storm_modes=mode_calls;
    for(unsigned i=0;i<100;++i) {
        event(NULL,IP_EVENT,IP_EVENT_STA_LOST_IP,NULL);step();
        event(NULL,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,NULL);step();
    }
    assert(mode_calls==storm_modes && !ap_enabled);
    clock_ms=5000;step();assert(wifi_mode==WIFI_MODE_STA && !ap_enabled);
    network_policy_started(&policy,clock_ms);network_policy_got_ip(&policy,"192.0.2.10",clock_ms);publish(false);
    associate_calls=0;
    clock_ms=0; network_policy_init(&policy,platform,&c,false,0);
    network_policy_started(&policy,0); network_policy_got_ip(&policy,"192.0.2.10",0);publish(false);
    associate_calls=0;
    at(0,1);at(40,1); // Stable release arms.
    at(100,0);at(140,0);at(5100,0);
    int modes=mode_calls, configs=config_calls;
    fill(); assert(network_finish_recovery()==ESP_ERR_INVALID_STATE && !pending);
    event(NULL,WIFI_EVENT,WIFI_EVENT_STA_START,NULL);assert(atomic_load(&overflow));
    // Release is sampled even with a full queue. Lost events are visible and
    // use the existing recovery policy; no NVS operation or extra task is needed.
    at(5100,1);fill();at(5140,1);
    assert(policy.status.manual_recovery && published.manual_recovery && published.recovery_ap);
    assert(published.event_queue_overflows==1 && !writes);
    assert(mode_calls==modes+1 && config_calls==configs && !associate_calls);
    while(qcount)step();
    // Finish can be queued while reconnecting, but only one action is pending.
    assert(network_finish_recovery()==ESP_OK && pending);
    assert(network_finish_recovery()==ESP_ERR_INVALID_STATE);
    step();assert(!pending && !published.manual_recovery && published.recovery_ap);
    // A mutex failure cannot claim admission or consume the pending reservation.
    mutex_ok=false;assert(network_finish_recovery()==ESP_ERR_INVALID_STATE && !pending);mutex_ok=true;
    // Second deliberate press after release. Failed AP opening is visible and paced.
    policy.status.state=NET_CONNECTED;policy.status.configured=true;policy.ready=true;
    strcpy(policy.status.ip,"192.0.2.10");assert(set_ap(false)==ESP_OK);policy.status.recovery_ap=false;
    at(6000,0);at(6040,0);at(11000,1);mode_error=ESP_FAIL;at(11040,1);
    assert(published.manual_recovery && !published.recovery_ap && !strcmp(published.recovery_error,"ap_open_failed"));
    modes=mode_calls;at(12039,1);assert(mode_calls==modes);
    mode_error=ESP_OK;at(12040,1);assert(published.recovery_ap && !strcmp(published.recovery_error,"none"));
    at(50000,1);assert(!writes && policy.status.manual_recovery);
    puts("Network adapter credentials/GPIO, full queue, one pending action, overflow and paced AP failure: passed");
}
