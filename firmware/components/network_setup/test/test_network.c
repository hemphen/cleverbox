// SPDX-License-Identifier: MIT
#include "network_policy.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static bool ap_on;
static unsigned starts, associates, stops, saves, forgets, ap_calls;
static esp_err_t start_error, save_error, forget_error, ap_error, associate_error;
static network_credentials persisted, last_start;
static esp_err_t ap(bool on) { ++ap_calls; if (!ap_error) ap_on = on; return ap_error; }
static esp_err_t start(const network_credentials *c) { ++starts; last_start = *c; return start_error; }
static esp_err_t associate(void) { ++associates; return associate_error; }
static void stop(void) { ++stops; }
static esp_err_t save(const network_credentials *c) { ++saves; if (!save_error) persisted = *c; return save_error; }
static esp_err_t forget(void) { ++forgets; if (!forget_error) memset(&persisted, 0, sizeof(persisted)); return forget_error; }
static network_platform platform = {ap, start, associate, stop, save, forget};
static network_credentials credentials(unsigned seed) {
    network_credentials c = {0};
    for (size_t i = 0; i < 12; ++i) { c.ssid[i] = (char)('A' + (seed + i) % 26); c.password[i] = (char)('a' + (seed + i * 3) % 26); }
    return c;
}
static void init(network_policy *p, const network_credentials *c, bool failed) {
    ap_on = false; network_policy_init(p, platform, c, failed, 0);
}
static void ip(network_policy *p, int64_t now) {
    network_policy_started(p, now); network_policy_got_ip(p, "192.0.2.10", now);
}
static int64_t fail_batch(network_policy *p, int64_t now) {
    for (unsigned i = 0; i < NETWORK_MAX_ATTEMPTS; ++i) {
        network_policy_started(p, now); network_policy_disconnected(p, now);
        unsigned n = starts;
        // Repeated lost-IP/disconnect and stale IP notifications consume no attempts.
        for (unsigned j = 0; j < 100; ++j) {
            network_policy_disconnected(p, now); network_policy_got_ip(p, "192.0.2.99", now);
        }
        assert(starts == n && !p->status.ip[0]);
        if (i + 1 < NETWORK_MAX_ATTEMPTS) {
            now += NETWORK_RETRY_MS;
            network_policy_tick(p, now - 1); assert(starts == n);
            network_policy_tick(p, now); assert(starts == n + 1);
        }
    }
    assert(p->status.state == NET_FAILED && p->status.attempts == NETWORK_MAX_ATTEMPTS);
    return now;
}
int main(void) {
    network_credentials a = credentials(1), b = credentials(2), bad = a;
    assert(network_credentials_valid(&a));
    memset(bad.ssid, 'a', sizeof(bad.ssid)); assert(!network_credentials_valid(&bad));
    bad = a; bad.password[7] = 0; assert(!network_credentials_valid(&bad));
    bad = a; bad.ssid[0] = '\n'; assert(!network_credentials_valid(&bad));
    bad = a; bad.password[0] = (char)128; assert(!network_credentials_valid(&bad));
    network_policy p;
    init(&p, NULL, false);
    assert(ap_on && !p.status.configured && !p.status.manual_recovery && !starts);
    network_policy_configure(&p, &a, 0);
    network_policy_got_ip(&p, "192.0.2.99", 1); network_policy_disconnected(&p, 1);
    assert(!saves && p.status.state == NET_CONNECTING);
    ip(&p, 100); assert(saves == 1 && !memcmp(&persisted, &a, sizeof(a)));
    network_policy_tick(&p, 60099); assert(ap_on && p.status.ap_closing);
    network_policy_disconnected(&p, 60100); // Closure is independent of connectivity.
    network_policy_tick(&p, 60100); assert(!ap_on && !p.status.ap_closing);
    // Finishing an in-flight setup keeps the explicit deadline even if DHCP
    // succeeds later. No network event may extend the warned closure window.
    init(&p, NULL, false); network_policy_configure(&p, &a, 0);
    network_policy_finish_recovery(&p, 1); ip(&p, 5000);
    network_policy_tick(&p, 60000); assert(ap_on);
    network_policy_tick(&p, 60001); assert(!ap_on);
    // Configured boot never opens AP, not even during repeated failed batches.
    init(&p, &a, false); unsigned opened = ap_calls;
    assert(!ap_on && p.status.configured && !memcmp(&last_start, &a, sizeof(a)));
    int64_t now = 0;
    for (unsigned batch = 0; batch < 4; ++batch) {
        now = fail_batch(&p, now);
        unsigned n = starts;
        network_policy_tick(&p, now + NETWORK_RECOVERY_RETRY_MS - 1); assert(starts == n);
        now += NETWORK_RECOVERY_RETRY_MS; network_policy_tick(&p, now);
        assert(starts == n + 1 && !ap_on && ap_calls == opened);
    }
    ip(&p, ++now); network_policy_disconnected(&p, ++now);
    assert(!ap_on && p.status.state == NET_DISCONNECTED);
    unsigned sv = saves, f = forgets, n = starts, st = stops;
    network_policy_recover(&p, ++now);
    assert(ap_on && p.status.manual_recovery && saves == sv && forgets == f && starts == n && stops == st);
    network_policy_finish_recovery(&p, ++now);
    network_policy_tick(&p, now + NETWORK_AP_GRACE_MS); assert(!ap_on);
    // Connected BOOT does not restart STA. Renewal doesn't release recovery.
    init(&p, &a, false); ip(&p, 1); n = starts; st = stops;
    network_policy_recover(&p, 2); ip(&p, 3); network_policy_tick(&p, 70000);
    assert(ap_on && p.status.manual_recovery && starts == n && stops == st);
    network_policy_finish_recovery(&p, 70001); network_policy_tick(&p, 130001); assert(!ap_on);
    // Failed connection replacement and fallback retain an authorised AP session.
    network_policy_recover(&p, 130002); network_policy_configure(&p, &b, 130003);
    now = fail_batch(&p, 130003);
    assert(ap_on && p.status.manual_recovery && p.status.setup == NET_SETUP_FAILED);
    network_policy_tick(&p, now + NETWORK_RECOVERY_RETRY_MS); ip(&p, now + NETWORK_RECOVERY_RETRY_MS + 1);
    network_policy_tick(&p, now + 200000); assert(ap_on && p.status.manual_recovery);
    // Write/read verification failures retain access, but never grant it on STA.
    for (unsigned access = 0; access < 2; ++access) {
        for (unsigned uncertain = 0; uncertain < 2; ++uncertain) {
            init(&p, &a, false); ip(&p, 1);
            if (access) network_policy_recover(&p, 2);
            save_error = uncertain ? NETWORK_ERR_PERSISTENCE_UNCERTAIN : ESP_FAIL;
            network_policy_configure(&p, &b, 3); ip(&p, 4);
            assert(ap_on == (bool)access && p.status.setup == NET_SETUP_FAILED);
            assert(p.status.persistence_uncertain == (bool)uncertain);
            assert(!memcmp(&p.saved, &a, sizeof(a)));
            network_policy_tick(&p, 60004); ip(&p, 60005); network_policy_tick(&p, 130000);
            assert(ap_on == (bool)access);
            if (access) {
                network_policy_finish_recovery(&p, 130001); network_policy_tick(&p, 190001);
                assert(!ap_on); // Explicit finish works even with uncertain persistence.
            }
            save_error = ESP_OK;
        }
    }
    // Corrupt/unreadable storage is not fresh setup and is never auto-erased.
    init(&p, NULL, true); sv = saves; f = forgets; n = starts;
    network_policy_tick(&p, 600000);
    assert(!ap_on && p.status.state == NET_FAILED && !strcmp(p.status.error, "storage_invalid"));
    assert(saves == sv && forgets == f && starts == n);
    network_policy_recover(&p, 600001); assert(ap_on);
    // Every explicit forget leaves access; only a successful erase drops RAM Wi-Fi.
    for (unsigned error = 0; error < 3; ++error) {
        init(&p, &a, false); ip(&p, 1);
        forget_error = error == 0 ? ESP_OK : error == 1 ? ESP_FAIL : NETWORK_ERR_PERSISTENCE_UNCERTAIN;
        network_policy_forget(&p, 2);
        assert(ap_on && p.status.manual_recovery && p.status.configured == (error != 0));
        network_policy_finish_recovery(&p, 3); network_policy_tick(&p, 60003); assert(!ap_on);
    }
    forget_error = ESP_OK;
    // Bounded AP open retries and close failure retry; no credential mutation.
    init(&p, &a, false); ap_error = ESP_FAIL; opened = ap_calls;
    network_policy_recover(&p, 1); assert(!ap_on && !strcmp(p.status.recovery_error, "ap_open_failed"));
    network_policy_tick(&p, 1000); assert(ap_calls == opened + 1);
    ap_error = ESP_OK; network_policy_tick(&p, 1001); assert(ap_on && !strcmp(p.status.recovery_error, "none"));
    network_policy_finish_recovery(&p, 1002); ap_error = ESP_FAIL;
    network_policy_tick(&p, 61002); assert(ap_on && p.status.ap_closing);
    ap_error = ESP_OK; network_policy_tick(&p, 62001); assert(ap_on);
    network_policy_tick(&p, 62002); assert(!ap_on);
    // Driver/association failures obey the same bounded retry policy, no AP.
    init(&p, &a, false); start_error = ESP_FAIL;
    network_policy_tick(&p, 15000); network_policy_tick(&p, 20000);
    network_policy_tick(&p, 25000); assert(p.status.state == NET_FAILED && !ap_on);
    start_error = ESP_OK; init(&p, &a, false); associate_error = ESP_FAIL;
    network_policy_started(&p, 1); assert(!p.attempting && !ap_on);
    associate_error = ESP_OK;
    puts("Network policy: deliberate access, offline completion, storms/batches, storage failures and BOOT isolation: passed");
}
