// SPDX-License-Identifier: MIT
// Production store against an NVS fault boundary: errors can take effect now or
// only after reinitialization. No production policy/storage logic is duplicated.
#include "network_setup.h"
#include "nvs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define LIMIT 256
static unsigned char durable[LIMIT], visible[LIMIT], deferred[LIMIT];
static size_t durable_size, visible_size, deferred_size;
static bool durable_present, visible_present, deferred_present, has_deferred;
static unsigned writes, commits, opens, reads;
static unsigned fail_write, fail_commit, fail_open, fail_read;
static bool mutate_error, defer_error, fail_recovery, mismatch_read;
static void copy(unsigned char *out, size_t *n, bool *present,
                 const unsigned char *in, size_t size, bool exists) {
    memcpy(out, in, size); *n = size; *present = exists;
}
static void reboot_nvs(void) {
    if (has_deferred) {
        copy(durable, &durable_size, &durable_present, deferred, deferred_size, deferred_present);
        has_deferred = false;
    }
    copy(visible, &visible_size, &visible_present, durable, durable_size, durable_present);
}
static void faults_clear(void) {
    writes = commits = opens = reads = 0;
    fail_write = fail_commit = fail_open = fail_read = 0;
    mutate_error = defer_error = fail_recovery = mismatch_read = false;
}
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *h) {
    assert(!strcmp(name, "cleverbox")); (void)mode;
    if (++opens == fail_open) return ESP_FAIL;
    *h = 1; return ESP_OK;
}
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *n) {
    (void)h; assert(!strcmp(key, "wifi"));
    if (++reads == fail_read) return ESP_FAIL;
    if (!visible_present) return ESP_ERR_NVS_NOT_FOUND;
    if (!out) { *n = visible_size; return ESP_OK; }
    if (*n < visible_size) return ESP_FAIL;
    memcpy(out, visible, visible_size); *n = visible_size;
    if (mismatch_read && writes && visible_size) ((unsigned char *)out)[0] ^= 1;
    return ESP_OK;
}
static esp_err_t mutate(const void *in, size_t n, bool present) {
    ++writes; assert(n <= LIMIT);
    if (writes == fail_write || (fail_recovery && fail_write && writes > fail_write)) {
        if (mutate_error && writes == fail_write) {
            copy(durable, &durable_size, &durable_present, in, n, present);
            copy(visible, &visible_size, &visible_present, in, n, present);
        }
        if (defer_error && writes == fail_write) {
            copy(deferred, &deferred_size, &deferred_present, in, n, present);
            has_deferred = true;
        }
        return ESP_FAIL;
    }
    copy(durable, &durable_size, &durable_present, in, n, present);
    copy(visible, &visible_size, &visible_present, in, n, present);
    // A successful restoring mutation resolves a pending failed operation.
    has_deferred = false;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *in, size_t n) {
    (void)h; assert(!strcmp(key, "wifi"));
    // ESP-IDF avoids rewriting equal cached values; a pending failed value can
    // still differ on flash. Rollback must force a write instead of trusting this.
    if (visible_present && visible_size == n && !memcmp(visible, in, n)) return ESP_OK;
    return mutate(in, n, true);
}
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key) {
    (void)h; assert(!strcmp(key, "wifi"));
    if (!visible_present) return ESP_ERR_NVS_NOT_FOUND;
    return mutate(visible, 0, false);
}
esp_err_t nvs_commit(nvs_handle_t h) {
    (void)h; return ++commits == fail_commit ? ESP_FAIL : ESP_OK;
}
static network_credentials creds(unsigned seed) {
    network_credentials c = {0};
    for (unsigned i = 0; i < 12; ++i) {
        c.ssid[i] = (char)('A' + (seed + i) % 26);
        c.password[i] = (char)('a' + (seed + i * 3) % 26);
    }
    return c;
}
static void expect(const network_credentials *wanted) {
    network_credentials actual;
    reboot_nvs();
    esp_err_t err = network_store_load(&actual);
    if (wanted) assert(err == ESP_OK && !memcmp(&actual, wanted, sizeof(actual)));
    else assert(err == ESP_ERR_NVS_NOT_FOUND);
}
static void seed(const network_credentials *c) {
    faults_clear(); assert(network_store_save(c) == ESP_OK); faults_clear();
}
int main(void) {
    network_credentials a = creds(1), b = creds(2);
    // Both set and commit errors can occur after the candidate became durable.
    seed(&a); fail_write = 1; mutate_error = true;
    assert(network_store_save(&b) == ESP_FAIL); faults_clear(); expect(&a);
    seed(&a); fail_commit = 1;
    assert(network_store_save(&b) == ESP_FAIL); faults_clear(); expect(&a);
    // Deferred effect on NVS reinitialization must also be superseded by rollback.
    seed(&a); fail_write = 1; defer_error = true;
    assert(network_store_save(&b) == ESP_FAIL); faults_clear(); expect(&a);
    // First-setup rollback restores absence when the failed value is visible.
    assert(network_store_forget() == ESP_OK); faults_clear();
    fail_write = 1; mutate_error = true;
    assert(network_store_save(&b) == ESP_FAIL); faults_clear(); expect(NULL);
    // Cached absence cannot prove that a deferred first-setup write is gone.
    fail_write = 1; defer_error = true;
    assert(network_store_save(&b) == NETWORK_ERR_PERSISTENCE_UNCERTAIN);
    faults_clear();
    assert(network_store_forget() == NETWORK_ERR_PERSISTENCE_UNCERTAIN);
    expect(&b); // The failure really did become effective at reinitialization.
    seed(&a);
    // Rollback mutation/open/readback failures must be reported as uncertain.
    fail_write = 1; mutate_error = true; fail_recovery = true;
    assert(network_store_save(&b) == NETWORK_ERR_PERSISTENCE_UNCERTAIN);
    faults_clear(); expect(&b); seed(&a);
    seed(&a); fail_write = 1; defer_error = true; fail_recovery = true;
    assert(network_store_save(&b) == NETWORK_ERR_PERSISTENCE_UNCERTAIN);
    faults_clear(); expect(&b); seed(&a);
    fail_write = 1; mutate_error = true; fail_open = 3;
    assert(network_store_save(&b) == NETWORK_ERR_PERSISTENCE_UNCERTAIN);
    faults_clear(); expect(&b); seed(&a);
    fail_write = 1; mutate_error = true; fail_read = 3;
    assert(network_store_save(&b) == NETWORK_ERR_PERSISTENCE_UNCERTAIN);
    faults_clear(); expect(&a); // Bytes restored but verification failed: still uncertain.
    seed(&a); mismatch_read = true;
    assert(network_store_save(&b) == NETWORK_ERR_PERSISTENCE_UNCERTAIN);
    faults_clear(); expect(&a);
    seed(&a); fail_write = 1; mutate_error = true; fail_commit = 1;
    assert(network_store_save(&b) == NETWORK_ERR_PERSISTENCE_UNCERTAIN);
    faults_clear(); expect(NULL); seed(&a);
    fail_write = 1; mutate_error = true; fail_commit = 2;
    assert(network_store_save(&b) == NETWORK_ERR_PERSISTENCE_UNCERTAIN);
    faults_clear(); expect(&a);
    // Explicit repair must not clear uncertainty with a same-value cached no-op.
    seed(&a); fail_write = 1; defer_error = true; fail_recovery = true;
    assert(network_store_save(&b) == NETWORK_ERR_PERSISTENCE_UNCERTAIN);
    faults_clear(); assert(network_store_save(&a) == ESP_OK); expect(&a);
    // Forget errors after erase/commit restore the previous record as well.
    seed(&a); fail_write = 1; mutate_error = true;
    assert(network_store_forget() == ESP_FAIL); faults_clear(); expect(&a);
    seed(&a); fail_commit = 1;
    assert(network_store_forget() == ESP_FAIL); faults_clear(); expect(&a);
    seed(&a); fail_write = 1; defer_error = true;
    assert(network_store_forget() == ESP_FAIL); faults_clear(); expect(&a);
    seed(&a); fail_write = 1; mutate_error = true; fail_recovery = true;
    assert(network_store_forget() == NETWORK_ERR_PERSISTENCE_UNCERTAIN);
    faults_clear(); expect(NULL); seed(&a);
    fail_write = 1; defer_error = true; fail_recovery = true;
    assert(network_store_forget() == NETWORK_ERR_PERSISTENCE_UNCERTAIN);
    faults_clear(); expect(NULL); seed(&a);
    // Errors before any mutation don't require rollback or touch storage.
    fail_open = 1; assert(network_store_save(&b) == ESP_FAIL && writes == 0);
    faults_clear(); expect(&a); faults_clear(); fail_read = 1;
    assert(network_store_forget() == ESP_FAIL && writes == 0); faults_clear(); expect(&a);
    // Success still persists and reset remains idempotent after verified recovery.
    seed(&b); expect(&b); assert(network_store_forget() == ESP_OK); expect(NULL);
    assert(network_store_forget() == ESP_OK); expect(NULL);
    // Boot reads distinguish absence from corrupt bytes and I/O errors without
    // mutating the key. No automatic repair or erase is authorised by a read.
    seed(&a); durable[0] = 255; reboot_nvs(); faults_clear();
    unsigned char corrupt[LIMIT]; memcpy(corrupt, durable, durable_size);
    network_credentials out;
    assert(network_store_load(&out) == ESP_ERR_INVALID_STATE && !writes && !commits);
    assert(!memcmp(durable, corrupt, durable_size));
    faults_clear(); fail_open = 1;
    assert(network_store_load(&out) == ESP_FAIL && !writes && !commits);
    faults_clear(); fail_read = 1;
    assert(network_store_load(&out) == ESP_FAIL && !writes && !commits);
    assert(!memcmp(durable, corrupt, durable_size));
    puts("Wi-Fi store: post-mutation/deferred errors, rollback/reinitialization, uncertain recovery, save/forget and key isolation: passed");
}
