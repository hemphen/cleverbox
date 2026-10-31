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
    if (p->platform.ap(enabled) == ESP_OK) {
        p->status.recovery_ap = enabled;
        // Opening can be retried by the owner after a platform failure.
        if (enabled) p->status.recovery_error = "none";
    }
    else p->status.error = "platform_failed";
}
static void failed_attempt(network_policy *p, int64_t now);
static void start(network_policy *p, int64_t now) {
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
    load_failed = load_failed || (saved && !network_credentials_valid(saved));
    p->status.error = load_failed ? "storage_invalid" : "none";
    p->status.recovery_error = "none";
    p->status.state = load_failed ? NET_FAILED : NET_DISCONNECTED;
    p->ap_requested = !load_failed && !saved;
    if (p->ap_requested) network_policy_recover(p, now);
    p->status.manual_recovery = false; // Fresh setup is not a button gesture.
    if (network_credentials_valid(saved)) {
        p->saved = *saved; p->status.configured = true; start(p, now);
    }
}
void network_policy_configure(network_policy *p, const network_credentials *c, int64_t now) {
    // Service admission rejects replacement while a connection attempt is active.
    if (!network_credentials_valid(c) || p->candidate_active) return;
    // A new explicit trial retains an already authorised access session.
    if (p->ap_requested) { p->close_ap_ms = 0; p->status.ap_closing = false; }
    p->candidate = *c; p->candidate_active = true;
    p->status.setup = NET_SETUP_PENDING; p->status.attempts = 0;
    if (!p->status.persistence_uncertain) p->status.error = "none";
    start(p, now);
}
static void storage_failure(network_policy *p, esp_err_t err) {
    if (err == NETWORK_ERR_PERSISTENCE_UNCERTAIN) p->status.persistence_uncertain = true;
    p->status.error = p->status.persistence_uncertain ? "persistence_uncertain" : "storage_failed";
    // Storage uncertainty never grants new wireless access. Existing access stays.
}
void network_policy_forget(network_policy *p, int64_t now) {
    esp_err_t err = p->platform.forget();
    // Forget always leaves recovery access, including a failed erase while
    // connected. Preserve the existing storage/rollback policy and RAM fallback.
    if (err != ESP_OK) { network_policy_recover(p, now); storage_failure(p, err); return; }
    p->platform.stop(); p->attempting = p->ready = p->candidate_active = false;
    memset(&p->saved, 0, sizeof(p->saved)); memset(&p->candidate, 0, sizeof(p->candidate));
    p->status = (network_status){.state = NET_DISCONNECTED, .error = "none",
        .recovery_ap = p->status.recovery_ap,
        .event_queue_overflows = p->status.event_queue_overflows,
        .recovery_error = "none"};
    network_policy_recover(p, now);
}
void network_policy_recover(network_policy *p, int64_t now) {
    p->ap_requested = true;
    p->status.manual_recovery = true;
    p->close_ap_ms = 0; p->status.ap_closing = false;
    // AP opening errors have their own status; do not overwrite an unrelated
    // storage/connection error or leave a stale platform_failed after retry.
    const char *previous_error = p->status.error;
    ap(p, true);
    p->status.error = previous_error;
    p->status.recovery_error = p->status.recovery_ap ? "none" : "ap_open_failed";
    p->recovery_retry_ms = now + 1000;
}
void network_policy_finish_recovery(network_policy *p, int64_t now) {
    p->status.manual_recovery = false;
    p->status.recovery_error = "none";
    // Explicit completion closes even the only access path, after UI warning.
    p->close_ap_ms = now + NETWORK_AP_GRACE_MS; p->status.ap_closing = true;
}
void network_policy_tick(network_policy *p, int64_t now) {
    if (p->attempting && now >= p->deadline_ms) failed_attempt(p, now);
    else if (!p->attempting && p->status.state != NET_CONNECTED &&
             (p->candidate_active || p->status.configured) && now >= p->next_ms) {
        if (p->status.attempts >= NETWORK_MAX_ATTEMPTS) p->status.attempts = 0;
        start(p, now);
    }
    if (p->ap_requested && !p->close_ap_ms && !p->status.recovery_ap && now >= p->recovery_retry_ms) {
        bool manual = p->status.manual_recovery;
        network_policy_recover(p, now); // Bounded one-second retries, visible failure.
        p->status.manual_recovery = manual;
    }
    if (p->close_ap_ms && now >= p->close_ap_ms) {
        p->ap_requested = false;
        ap(p, false);
        if (!p->status.recovery_ap) { p->close_ap_ms = 0; p->status.ap_closing = false; }
        else p->close_ap_ms = now + 1000; // Pace failed close retries too.
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
        p->next_ms = now + NETWORK_RETRY_MS;
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
        p->status.manual_recovery = false;
        p->status.recovery_error = "none";
        memset(&p->candidate, 0, sizeof(p->candidate)); p->candidate_active = false;
        p->status.setup = NET_SETUP_SAVED;
        if (p->ap_requested && !p->close_ap_ms) { p->close_ap_ms = now + NETWORK_AP_GRACE_MS; p->status.ap_closing = true; }
    }
    p->status.state = NET_CONNECTED;
    if (p->status.setup != NET_SETUP_FAILED && !p->status.persistence_uncertain) p->status.error = "none";
    strcpy(p->status.ip, ip); p->attempting = false;
    // Ordinary reconnects neither grant access nor close an authorised session.
}
const char *network_state_name(network_state s) {
    static const char *names[] = {"disconnected", "connecting", "connected", "failed"}; return names[s];
}
const char *network_setup_name(network_setup_result s) {
    static const char *names[] = {"none", "pending", "saved", "failed"}; return names[s];
}
