// SPDX-License-Identifier: MIT
#include "network_policy.h"
#include <string.h>

bool network_credentials_valid(const network_credentials *c) {
    if (!c) return false;
    const char *s = memchr(c->ssid, 0, sizeof(c->ssid));
    const char *p = memchr(c->password, 0, sizeof(c->password));
    if (!s || !p || s == c->ssid || p - c->password < 8) return false;
    for (const unsigned char *v = (const unsigned char *)c->ssid; *v; ++v)
        if (*v < 32 || *v == 127) return false;
    for (const unsigned char *v = (const unsigned char *)c->password; *v; ++v)
        if (*v < 32 || *v > 126) return false;
    return true;
}
static void ap(network_policy *p, bool enabled) {
    if (p->status.recovery_ap == enabled) return;
    if (p->platform.ap(enabled) == ESP_OK) p->status.recovery_ap = enabled;
    else p->status.error = "platform_failed";
}
static void failed_attempt(network_policy *p, int64_t now);
static void start(network_policy *p, int64_t now) {
    ap(p, true);
    p->close_ap_ms = 0;
    p->ready = false; p->attempting = true;
    p->status.state = NET_CONNECTING; p->status.ip[0] = 0;
    ++p->status.attempts;
    p->deadline_ms = now + NETWORK_ATTEMPT_MS;
    if (p->platform.start(p->candidate_active ? &p->candidate : &p->saved) != ESP_OK)
        failed_attempt(p, now);
}
static void failed_attempt(network_policy *p, int64_t now) {
    p->ready = p->attempting = false; p->platform.stop();
    p->status.ip[0] = 0;
    if (p->status.attempts < NETWORK_MAX_ATTEMPTS) {
        p->status.state = NET_DISCONNECTED; p->next_ms = now + NETWORK_RETRY_MS;
        return;
    }
    p->status.state = NET_FAILED;
    if (!strcmp(p->status.error, "none")) p->status.error = "connection_failed";
    if (p->candidate_active) {
        p->candidate_active = false;
        memset(&p->candidate, 0, sizeof(p->candidate));
        p->status.setup = NET_SETUP_FAILED;
    }
    // Keep the saved RAM fallback after failed replacement. Persistence errors
    // distinguish a verified rollback from an uncertain restart outcome. Reconnect in
    // bounded batches, even after DHCP timeout or a router disappearing later.
    p->next_ms = now + NETWORK_RECOVERY_RETRY_MS;
}
void network_policy_init(network_policy *p, network_platform platform,
                         const network_credentials *saved, bool load_failed, int64_t now) {
    memset(p, 0, sizeof(*p)); p->platform = platform;
    p->status.error = load_failed ? "storage_invalid" : "none";
    p->status.state = load_failed ? NET_FAILED : NET_DISCONNECTED;
    ap(p, true);
    if (network_credentials_valid(saved)) {
        p->saved = *saved; p->status.configured = true; start(p, now);
    }
}
void network_policy_configure(network_policy *p, const network_credentials *c, int64_t now) {
    // Service admission rejects replacement while a connection attempt is active.
    if (!network_credentials_valid(c) || p->candidate_active) return;
    p->candidate = *c; p->candidate_active = true;
    p->status.setup = NET_SETUP_PENDING; p->status.attempts = 0;
    if (!p->status.persistence_uncertain) p->status.error = "none";
    start(p, now);
}
static void storage_failure(network_policy *p, esp_err_t err) {
    if (err == NETWORK_ERR_PERSISTENCE_UNCERTAIN) p->status.persistence_uncertain = true;
    p->status.error = p->status.persistence_uncertain ? "persistence_uncertain" : "storage_failed";
    if (p->status.persistence_uncertain) { p->close_ap_ms = 0; ap(p, true); }
}
void network_policy_forget(network_policy *p, int64_t now) {
    (void)now;
    esp_err_t err = p->platform.forget();
    if (err != ESP_OK) { storage_failure(p, err); return; }
    p->platform.stop(); p->attempting = p->ready = p->candidate_active = false;
    memset(&p->saved, 0, sizeof(p->saved)); memset(&p->candidate, 0, sizeof(p->candidate));
    p->status = (network_status){.state = NET_DISCONNECTED, .error = "none", .recovery_ap = p->status.recovery_ap};
    p->close_ap_ms = 0; ap(p, true);
}
void network_policy_tick(network_policy *p, int64_t now) {
    if (p->attempting && now >= p->deadline_ms) failed_attempt(p, now);
    else if (!p->attempting && p->status.state != NET_CONNECTED &&
             (p->candidate_active || p->status.configured) && now >= p->next_ms) {
        if (p->status.attempts >= NETWORK_MAX_ATTEMPTS) p->status.attempts = 0;
        start(p, now);
    }
    if (p->status.state == NET_CONNECTED && p->close_ap_ms && now >= p->close_ap_ms) {
        ap(p, false);
        if (!p->status.recovery_ap) p->close_ap_ms = 0;
    }
}
void network_policy_started(network_policy *p, int64_t now) {
    if (!p->attempting || p->ready) return;
    p->ready = true;
    if (p->platform.associate() != ESP_OK) failed_attempt(p, now);
}
void network_policy_disconnected(network_policy *p, int64_t now) {
    if (p->status.state == NET_CONNECTED) {
        p->status.ip[0] = 0; p->status.state = NET_DISCONNECTED;
        p->ready = false; p->status.attempts = 0;
        p->next_ms = now + NETWORK_RETRY_MS; ap(p, true);
    } else if (p->attempting && p->ready) failed_attempt(p, now);
}
void network_policy_got_ip(network_policy *p, const char *ip, int64_t now) {
    if (!ip || !*ip || strlen(ip) >= sizeof(p->status.ip)) return;
    if (p->status.state == NET_CONNECTED && p->ready) {
        // DHCP renewal can change the address without a disconnect. No new save
        // or AP grace period is needed for an already established connection.
        strcpy(p->status.ip, ip); return;
    }
    if (!p->attempting || !p->ready) return;
    if (p->candidate_active) {
        esp_err_t err = p->platform.save(&p->candidate);
        if (err != ESP_OK) {
            storage_failure(p, err);
            p->status.attempts = NETWORK_MAX_ATTEMPTS;
            failed_attempt(p, now); return;
        }
        p->saved = p->candidate; p->status.configured = true;
        p->status.persistence_uncertain = false;
        memset(&p->candidate, 0, sizeof(p->candidate)); p->candidate_active = false;
        p->status.setup = NET_SETUP_SAVED;
    }
    p->status.state = NET_CONNECTED;
    if (p->status.setup != NET_SETUP_FAILED && !p->status.persistence_uncertain) p->status.error = "none";
    strcpy(p->status.ip, ip); p->attempting = false;
    p->close_ap_ms = p->status.persistence_uncertain ? 0 : now + NETWORK_AP_GRACE_MS;
}
const char *network_state_name(network_state s) {
    static const char *names[] = {"disconnected", "connecting", "connected", "failed"}; return names[s];
}
const char *network_setup_name(network_setup_result s) {
    static const char *names[] = {"none", "pending", "saved", "failed"}; return names[s];
}
