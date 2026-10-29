// SPDX-License-Identifier: MIT
#include "network_policy.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static bool ap_on;
static unsigned starts, associates, stops, saves, forgets;
static esp_err_t start_error, save_error, forget_error, ap_error, associate_error;
static network_credentials persisted, last_start;
static esp_err_t ap(bool on) { if (!ap_error) ap_on = on; return ap_error; }
static esp_err_t start(const network_credentials *c) { ++starts; last_start = *c; return start_error; }
static esp_err_t associate(void) { ++associates; return associate_error; }
static void stop(void) { ++stops; }
static esp_err_t save(const network_credentials *c) { ++saves; if (!save_error) persisted = *c; return save_error; }
static esp_err_t forget(void) { ++forgets; if (!forget_error) memset(&persisted, 0, sizeof(persisted)); return forget_error; }
static network_platform platform = {ap, start, associate, stop, save, forget};
static network_credentials credentials(unsigned seed) {
    network_credentials c = {0};
    // Generated stand-ins only; no credential fixture or secret assertion output.
    for (size_t i = 0; i < 12; ++i) { c.ssid[i] = (char)('A' + (seed + i) % 26); c.password[i] = (char)('a' + (seed + i * 3) % 26); }
    return c;
}
static void ip(network_policy *p, int64_t now) {
    network_policy_started(p, now); network_policy_got_ip(p, "192.0.2.10", now);
}
int main(void) {
    network_credentials a = credentials(1), b = credentials(2), bad = a;
    assert(network_credentials_valid(&a));
    memset(bad.ssid, 'a', sizeof(bad.ssid)); assert(!network_credentials_valid(&bad));
    bad = a; bad.password[7] = 0; assert(!network_credentials_valid(&bad));
    bad = a; bad.ssid[0] = '\n'; assert(!network_credentials_valid(&bad));
    bad = a; bad.password[0] = (char)128; assert(!network_credentials_valid(&bad));
    network_policy p;
    network_policy_init(&p, platform, NULL, false, 0);
    assert(ap_on && p.status.state == NET_DISCONNECTED && !p.status.configured && starts == 0);
    network_policy_configure(&p, &a, 0);
    assert(p.status.state == NET_CONNECTING && ap_on && saves == 0);
    // Old IP/disconnect events before fresh STA_START must not save credentials.
    network_policy_got_ip(&p, "192.0.2.99", 1); network_policy_disconnected(&p, 1);
    assert(p.status.state == NET_CONNECTING && saves == 0);
    ip(&p, 100);
    assert(saves == 1 && !memcmp(&persisted, &a, sizeof(a)) && p.status.configured);
    assert(p.status.state == NET_CONNECTED && p.status.setup == NET_SETUP_SAVED);
    network_policy_tick(&p, 60099); assert(ap_on);
    network_policy_tick(&p, 60100); assert(!ap_on);
    network_policy_got_ip(&p, "192.0.2.11", 60101);
    assert(!strcmp(p.status.ip, "192.0.2.11") && !ap_on && saves == 1);
    // Restart uses saved config without a new save or automatic RF actions.
    network_policy_init(&p, platform, &persisted, false, 70000);
    assert(ap_on && !memcmp(&last_start, &a, sizeof(a)));
    ip(&p, 70001); assert(saves == 1);
    // Router loss: recovery immediately, then a 5-second pace before reconnect.
    network_policy_tick(&p, 130001); assert(!ap_on);
    network_policy_disconnected(&p, 130002); unsigned n = starts;
    assert(ap_on && !p.status.ip[0] && p.status.state == NET_DISCONNECTED);
    network_policy_tick(&p, 135001); assert(starts == n);
    network_policy_tick(&p, 135002); assert(starts == n + 1);
    ip(&p, 135003); assert(p.status.state == NET_CONNECTED && saves == 1);
    // Failed replacement: three full DHCP deadlines, paced, previous record intact.
    network_policy_configure(&p, &b, 140000);
    for (int i = 0; i < 3; ++i) {
        int64_t t = 140000 + i * 20000;
        network_policy_started(&p, t);
        network_policy_tick(&p, t + 14999); assert(p.status.state == NET_CONNECTING);
        network_policy_tick(&p, t + 15000);
        assert(ap_on && !memcmp(&persisted, &a, sizeof(a)));
        if (i < 2) {
            n = starts; network_policy_tick(&p, t + 19999); assert(starts == n);
            network_policy_tick(&p, t + 20000); assert(starts == n + 1);
        }
    }
    assert(p.status.state == NET_FAILED && p.status.setup == NET_SETUP_FAILED && p.status.configured);
    n = starts; network_policy_tick(&p, 254999); assert(starts == n);
    network_policy_tick(&p, 255000); assert(starts == n + 1 && !memcmp(&last_start, &a, sizeof(a)));
    ip(&p, 255001); assert(p.status.state == NET_CONNECTED && p.status.setup == NET_SETUP_FAILED);
    // NVS failure after candidate DHCP also rolls back and keeps recovery access.
    save_error = ESP_FAIL; network_policy_configure(&p, &b, 260000); ip(&p, 260001);
    assert(p.status.state == NET_FAILED && !strcmp(p.status.error, "storage_failed"));
    assert(!memcmp(&persisted, &a, sizeof(a)) && ap_on); save_error = ESP_OK;
    // Forget failure retains setup and reports the storage failure.
    forget_error = ESP_FAIL; network_policy_forget(&p, 260002);
    assert(p.status.configured && !memcmp(&persisted, &a, sizeof(a))); forget_error = ESP_OK;
    network_policy_forget(&p, 260003);
    assert(!p.status.configured && p.status.state == NET_DISCONNECTED && ap_on);
    n = starts; network_policy_tick(&p, 999999); assert(starts == n);
    // Late IP after forget cannot re-save or reconnect.
    ip(&p, 1000000); assert(!p.status.configured && p.status.state == NET_DISCONNECTED);
    // Initial invalid credentials fail, without manufacturing a saved setup.
    network_policy_configure(&p, &b, 1000010);
    for (int i = 0; i < 3; ++i) {
        network_policy_started(&p, 1000010 + i * 5001);
        network_policy_disconnected(&p, 1000011 + i * 5001);
        if (i < 2) network_policy_tick(&p, 1005011 + i * 5001);
    }
    assert(p.status.state == NET_FAILED && !p.status.configured && ap_on);
    // Immediate driver errors are also bounded, including a missing STA_START.
    start_error = ESP_FAIL; network_policy_configure(&p, &a, 1100000);
    assert(p.status.state == NET_DISCONNECTED);
    network_policy_tick(&p, 1105000); network_policy_tick(&p, 1110000);
    assert(p.status.state == NET_FAILED); start_error = ESP_OK;
    network_policy_init(&p, platform, NULL, true, 0);
    assert(p.status.state == NET_FAILED && !strcmp(p.status.error, "storage_invalid") && ap_on);
    // Recover from corrupt storage by explicit successful replacement.
    network_policy_configure(&p, &a, 1); ip(&p, 2); assert(p.status.configured);
    assert(associates && stops && saves && forgets);
    puts("Network policy setup/restart, rollback, loss/recovery, pacing, storage failures and reset: passed");
}
