// SPDX-License-Identifier: MIT
#ifdef CLEVERBOX_HOST
#include "platform.h"
#else
#include "idf_hal.h"
#endif
#include "radio_transport.h"
#include "esp_log.h"
#include "device_store.h"
#include "device_time.h"
#include "freertos/queue.h"
#ifndef CLEVERBOX_HOST
#include "freertos/semphr.h"
#include "esp_random.h"
#endif
#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
// Component-manager Git locks can retain a previous revision until explicitly
// updated. Fail compilation if the build resolves a different release.
static_assert(RADIOLIB_VERSION == 0x07080100, "Expected pinned RadioLib 7.8.1");
#endif

static IdfHal hal;
static Module module(&hal, RADIO_CS, RADIO_GDO0, RADIOLIB_NC, RADIO_GDO2);
static CC1101 radio(&module);
static radio_status status = {};
// Task mutex: admission validates/copies frames and calls the nonblocking queue
// API here. Never disable interrupts around FreeRTOS APIs or codec work.
static SemaphoreHandle_t status_lock;
static void lock_status() { if (status_lock) xSemaphoreTake(status_lock, portMAX_DELAY); }
static void unlock_status() { if (status_lock) xSemaphoreGive(status_lock); }
static bool tx_in_progress;
static radio_observation observations[RADIO_PASSIVE_CAPACITY];
static monitor_registry monitors;
static radio_monitor monitored[RADIO_MONITOR_CAPACITY];
static radio_candidate candidates[RADIO_PASSIVE_CAPACITY];
static uint64_t capture_sequence, capture_boot;


extern "C" radio_status radio_transport_status(void)
{
    lock_status();
    radio_status copy = status;
    unlock_status();
    return copy;
}

static radiator_registry registry;
static bool mutation_pending;
static uint32_t exchange_id; // Pairing reservation only; commands own per-record reservations.
static bool pair_reserved;
static bool exchange_pending;
static radiator_record *find_device(uint32_t id) {
    if (!id && registry.count == 1) id = registry.records[0].id;
    for (size_t i = 0; i < registry.count; ++i)
        if (id && registry.records[i].id == id) return &registry.records[i];
    return nullptr;
}
extern "C" esp_err_t radio_device_status(uint32_t id, radiator_record *out) {
    lock_status();
    radiator_record *d = find_device(id);
    if (d) *out = *d;
    unlock_status();
    return d ? ESP_OK : ESP_ERR_NOT_FOUND;
}
extern "C" esp_err_t radio_device_at(size_t index, radiator_record *out) {
    lock_status();
    bool found = index < registry.count;
    if (found) *out = registry.records[index];
    unlock_status();
    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}
enum radio_action { PAIR_START, PAIR_CANCEL, PAIR_SAVE, CONTROL_START, STATE_SAVE, STATE_CLEAR, RENAME, REMOVE, MODE_SET, MONITOR_SET, MONITOR_REMOVE };
struct radio_request {
    radio_action action; uint32_t id;
    bool enabled; char adoption_version[33];
    uint8_t controller[4], candidate[4];
    uint8_t state[CONTROLLER_STATE_BYTES];
    rf_mode mode; int centi_c; unsigned clock_seconds;
    int64_t admitted_ms;
    char name[RADIATOR_NAME_BYTES + 1];
};
static QueueHandle_t radio_requests;
static esp_err_t queue_request(const radio_request &r) {
    if (xQueueSend(radio_requests, &r, 0) == pdPASS) return ESP_OK;
    // Caller still holds admission lock; undo only this request's reservation.
    return ESP_ERR_TIMEOUT;
}
static void increment(uint32_t &n) { if (n != UINT32_MAX) ++n; }
extern "C" const char *radio_health_name(radio_health h) {
    switch (h) { case RADIO_INITIALIZING: return "initializing"; case RADIO_RECOVERING: return "recovering";
        case RADIO_READY: return "ready"; default: return "faulted"; }
}
extern "C" const char *radio_failure_name(radio_failure_stage s) {
    switch (s) { case RADIO_FAILURE_INITIALIZE: return "initialize"; case RADIO_FAILURE_RECEIVE: return "receive";
        case RADIO_FAILURE_TX: return "transmit"; case RADIO_FAILURE_RX_RESTORE: return "rx_restore";
        case RADIO_FAILURE_TX_START: return "tx_start"; case RADIO_FAILURE_TX_COMPLETE: return "tx_complete";
        case RADIO_FAILURE_RESOURCES: return "resources"; default: return "none"; }
}
extern "C" esp_err_t radio_retry(void) {
    lock_status();
    esp_err_t e = ESP_ERR_INVALID_STATE;
    if (status.worker_running && status.health == RADIO_FAULTED && !status.retry_pending) {
        status.retry_pending = true; e = ESP_OK;
    }
    unlock_status(); return e;
}
static bool rf_allowed() { return status.control_enabled && !status.mode_pending && !status.mode_error; }
extern "C" esp_err_t radio_mode_set(bool enabled) {
    radio_request r = {}; r.action = MODE_SET; r.enabled = enabled;
    lock_status();
    bool busy = pair_reserved || mutation_pending || status.mode_pending || tx_in_progress;
    for (size_t i = 0; i < registry.count; ++i) busy |= registry.records[i].command_reserved;
    esp_err_t e = RADIO_ERR_MODE_BUSY;
    if (!busy && status.worker_running && radio_requests) {
        status.mode_pending = true;
        e = queue_request(r);
        if (e == ESP_OK) status.mode_save_result = RADIO_STORE_PENDING;
        else status.mode_pending = false;
    }
    unlock_status(); return e;
}
extern "C" esp_err_t radio_observation_at(size_t index, radio_observation *out) {
    lock_status(); bool found = index < status.observation_count;
    if (found) *out = observations[index];
    unlock_status(); return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}
extern "C" esp_err_t radio_candidate_at(size_t index, radio_candidate *out) {
    lock_status(); bool found = index < status.candidate_count;
    if (found) *out = candidates[index];
    unlock_status(); return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}
extern "C" esp_err_t radio_monitor_at(size_t i, radio_monitor *out) {
    lock_status(); bool found=i<monitors.count;
    if (found) *out=monitored[i];
    unlock_status(); return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}
extern "C" esp_err_t radio_monitor_get(const uint8_t a[4], radio_monitor *out) {
    lock_status(); bool found=false;
    for (size_t i=0;i<monitors.count;++i) if (!memcmp(a,monitors.records[i].address,4)) { *out=monitored[i]; found=true; break; }
    unlock_status(); return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}
extern "C" esp_err_t radio_monitor_change(const char *action, const uint8_t a[4], const char *name) {
    bool remove=!strcmp(action,"remove"), select=!strcmp(action,"select"), rename=!strcmp(action,"rename");
    if ((!remove && !select && !rename) || (!remove && (!name || !*name || strlen(name)>RADIATOR_NAME_BYTES))) return ESP_ERR_INVALID_ARG;
    if (!remove) for (const char *c=name;*c;++c) if ((unsigned char)*c<32 || *c==127) return ESP_ERR_INVALID_ARG;
    radio_request r={}; r.action=remove ? MONITOR_REMOVE : MONITOR_SET; memcpy(r.candidate,a,4);
    if (!remove) strcpy(r.name,name);
    lock_status();
    bool found=false, observed=false, paired=false;
    for (size_t i=0;i<monitors.count;++i) found|=!memcmp(a,monitors.records[i].address,4);
    for (size_t i=0;i<status.observation_count;++i) observed|=!memcmp(a,observations[i].report.source,4);
    for (size_t i=0;i<registry.count;++i) paired|=!memcmp(a,registry.records[i].pair.radiator,4);
    esp_err_t e=paired ? RADIO_ERR_MONITOR_PAIRED :
        !found && select && monitors.count==RADIO_MONITOR_CAPACITY ? RADIO_ERR_MONITOR_CAPACITY :
        !found && select && !observed ? RADIO_ERR_MONITOR_UNOBSERVED : ESP_ERR_INVALID_STATE;
    if (radio_requests && status.worker_running && !mutation_pending && !status.mode_pending && !pair_reserved &&
        !status.monitor_error && !status.registry_error && !paired &&
        (found || (select && observed && monitors.count<RADIO_MONITOR_CAPACITY))) {
        mutation_pending=status.monitor_pending=true; e=queue_request(r);
        if (e!=ESP_OK) mutation_pending=status.monitor_pending=false;
        else status.monitor_result=RADIO_STORE_PENDING;
    }
    unlock_status(); return e;
}
// Owner calls under task mutex. Lowest timestamp wins; ties use the lowest
// slot. Reports and controller candidates have independent replacement bounds.
static void capture_passive(const rf_frame &f, const rf_report *report, bool state, int64_t now) {
    if (report) {
        for (size_t i=0;i<monitors.count;++i) if (!memcmp(monitors.records[i].address,report->source,4)) {
            monitored[i].available=true; monitored[i].observation={*report,now};
        }
        size_t slot = status.observation_count;
        for (size_t i = 0; i < status.observation_count; ++i)
            if (!memcmp(observations[i].report.source, report->source, 4)) { slot = i; break; }
        if (slot == RADIO_PASSIVE_CAPACITY) {
            slot = 0;
            for (size_t i = 1; i < RADIO_PASSIVE_CAPACITY; ++i)
                if (observations[i].time_us < observations[slot].time_us) slot = i;
            if (status.observation_evictions != UINT32_MAX) ++status.observation_evictions;
        } else if (slot == status.observation_count) ++status.observation_count;
        observations[slot] = {*report, now};
    }
    if (state && capture_sequence != UINT64_MAX) {
        size_t slot = status.candidate_count;
        for (size_t i = 0; i < status.candidate_count; ++i)
            if (!memcmp(candidates[i].state + 6, f.bytes + 6, 4)) { slot = i; break; }
        if (slot == RADIO_PASSIVE_CAPACITY) {
            slot = 0;
            for (size_t i = 1; i < RADIO_PASSIVE_CAPACITY; ++i)
                if (candidates[i].time_us < candidates[slot].time_us) slot = i;
            if (status.candidate_evictions != UINT32_MAX) ++status.candidate_evictions;
        } else if (slot == status.candidate_count) ++status.candidate_count;
        auto &c = candidates[slot]; memcpy(c.state, f.bytes, CONTROLLER_STATE_BYTES); c.time_us = now;
        // Even identical bytes get a fresh version: a selection names one exact
        // capture, never a reusable slot. Refuse wrapping the counter.
        snprintf(c.version, sizeof(c.version), "%016llX%016llX",
                 (unsigned long long)capture_boot, (unsigned long long)++capture_sequence);
    }
}
extern "C" esp_err_t radio_pair_start(const uint8_t controller[4], const uint8_t candidate[4], uint32_t *id) {
    rf_pairing check = {};
    if (!rf_pair_start(&check, controller, candidate, esp_timer_get_time() / 1000)) return ESP_ERR_INVALID_ARG;
    radio_request r = {}; r.action = PAIR_START; r.admitted_ms = esp_timer_get_time() / 1000;
    memcpy(r.controller, controller, 4); memcpy(r.candidate, candidate, 4);
    lock_status();
    esp_err_t result = rf_allowed() ? ESP_ERR_INVALID_STATE : RADIO_ERR_LISTEN_ONLY;
    if (rf_allowed() && status.ready && radio_requests && !status.registry_error && !status.monitor_error && !exchange_id && !mutation_pending &&
        registry.count < CLEVERBOX_RADIATOR_CAPACITY && registry.next_id != UINT32_MAX) {
        bool commands_outstanding = false;
        for (size_t i = 0; i < registry.count; ++i) commands_outstanding |= registry.records[i].command_reserved;
        if (!commands_outstanding) {
            r.id = registry.next_id; exchange_id = r.id; pair_reserved = true; exchange_pending = true;
            result = queue_request(r);
            if (result != ESP_OK) { exchange_id = 0; pair_reserved = false; exchange_pending = false; }
            else { status.pairing_duplicate = false; status.pairing_id = r.id; status.exchange_id = r.id; *id = r.id; }
        }
    }
    unlock_status(); return result;
}
static esp_err_t pair_action(uint32_t id, radio_action action) {
    radio_request r = {}; r.action = action;
    lock_status();
    // Legacy no-id action is allowed only for the unambiguous pending inclusion.
    if (!id && registry.count <= 1) id = status.pairing_id;
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (status.ready && radio_requests && pair_reserved && id == exchange_id &&
        (action != PAIR_SAVE || (!exchange_pending && !status.pairing_saved && status.pairing.phase == RF_PAIR_COMPLETE))) {
        r.id = id; result = queue_request(r);
    }
    unlock_status(); return result;
}
extern "C" esp_err_t radio_pair_cancel(uint32_t id) { return pair_action(id, PAIR_CANCEL); }
extern "C" esp_err_t radio_pair_save(uint32_t id) { return pair_action(id, PAIR_SAVE); }
static esp_err_t control_start_at(uint32_t id, const rf_control_request *request, int64_t admitted_ms, uint64_t cookie = 0) {
    if (!request) return ESP_ERR_INVALID_ARG;
    radio_request r = {}; r.action = CONTROL_START; r.admitted_ms = admitted_ms; rf_control_request chosen = *request;
    lock_status();
    radiator_record *d = find_device(id);
    esp_err_t result = rf_allowed() ? ESP_ERR_INVALID_STATE : RADIO_ERR_LISTEN_ONLY;
    if (d && rf_allowed() && status.ready && radio_requests && !status.registry_error && !pair_reserved && !d->command_reserved && !mutation_pending) {
        if (!chosen.state.size) {
            const uint8_t *bytes = d->has_control_template ? d->control_template :
                d->has_saved_template ? d->saved_template : nullptr;
            if (bytes) { chosen.state.size = CONTROLLER_STATE_BYTES; memcpy(chosen.state.bytes, bytes, CONTROLLER_STATE_BYTES); }
        }
        if (chosen.state.size) {
            rf_control check = {};
            if (!rf_control_start(&check, &chosen, d->pair.controller, d->pair.radiator, admitted_ms))
                result = ESP_ERR_INVALID_ARG;
            else {
                r.id = d->id; memcpy(r.state, chosen.state.bytes, CONTROLLER_STATE_BYTES);
                r.mode = chosen.mode; r.centi_c = chosen.centi_c; r.clock_seconds = chosen.clock_seconds;
                rf_control_status previous = d->control; int previous_error = d->control_error;
                d->command_reserved = d->command_pending = true;
                d->control = check.status; d->control_error = 0;
                result = queue_request(r);
                if (result != ESP_OK) {
                    d->command_reserved = d->command_pending = false;
                    d->control = previous; d->control_error = previous_error;
                } else {
                    d->command_cookie = cookie;
                    if (cookie) { d->result_cookie = cookie; d->tracked_result = d->control; d->tracked_error = 0; }
                }
            }
        }
    }
    unlock_status(); return result;
}
extern "C" esp_err_t radio_control_start(uint32_t id, const rf_control_request *request) {
    return control_start_at(id, request, esp_timer_get_time() / 1000);
}
extern "C" esp_err_t radio_control_start_device(uint32_t id, const rf_control_request *request) {
    return radio_control_start_device_tracked(id, request, 0);
}
extern "C" esp_err_t radio_control_start_device_tracked(uint32_t id, const rf_control_request *request, uint64_t cookie) {
    if (!request) return ESP_ERR_INVALID_ARG;
    lock_status(); bool allowed = rf_allowed(); unlock_status();
    if (!allowed) return RADIO_ERR_LISTEN_ONLY;
    rf_control_request chosen = *request; int64_t sampled_ms;
    if (!device_time_command_clock(&chosen.clock_seconds, &sampled_ms)) return RADIO_ERR_TIME_UNAVAILABLE;
    return control_start_at(id, &chosen, sampled_ms, cookie);
}
static esp_err_t enqueue_mutation(uint32_t id, radio_action action, const rf_frame *imported, const char *name, const uint8_t *radiator = nullptr, const char *version = nullptr, bool replace = false) {
    radio_request r = {}; r.action = action;
    if (name) {
        if (!*name || strlen(name) > RADIATOR_NAME_BYTES) return ESP_ERR_INVALID_ARG;
        for (const char *c = name; *c; ++c) if ((unsigned char)*c < 32 || *c == 127) return ESP_ERR_INVALID_ARG;
        memcpy(r.name, name, strlen(name) + 1);
    }
    lock_status();
    radiator_record *d = find_device(id);
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (d && status.worker_running && radio_requests && !mutation_pending && !status.mode_pending &&
        (!status.registry_error || (d->state_load_error && (action == STATE_SAVE || action == STATE_CLEAR))) &&
        !(action == REMOVE && d->command_reserved)) {
        result = ESP_OK; r.id = d->id;
        rf_frame captured = {};
        if (version) {
            result = RADIO_ERR_CANDIDATE_CHANGED;
            for (size_t i = 0; i < status.candidate_count; ++i) {
                const auto &c = candidates[i];
                if (!strcmp(c.version, version) && !memcmp(c.state + 6, radiator, 4)) {
                    captured.size = CONTROLLER_STATE_BYTES; memcpy(captured.bytes, c.state, CONTROLLER_STATE_BYTES);
                    imported = &captured; result = ESP_OK; break;
                }
            }
            if (result == ESP_OK && memcmp(radiator, d->pair.radiator, 4)) result = ESP_ERR_INVALID_ARG;
            if (result == ESP_OK && d->has_saved_template && !replace) result = RADIO_ERR_REPLACE_REQUIRED;
            if (result != ESP_OK) { unlock_status(); return result; }
            memcpy(r.adoption_version, version, sizeof(r.adoption_version));
        }
        if (action == STATE_SAVE) {
            if (imported) {
                if (imported->size == CONTROLLER_STATE_BYTES) memcpy(r.state, imported->bytes, CONTROLLER_STATE_BYTES);
                else result = ESP_ERR_INVALID_ARG;
            }
            else if (d->has_control_template) {
                memcpy(r.state, d->control_template, CONTROLLER_STATE_BYTES);
            } else result = ESP_ERR_INVALID_STATE;
            rf_frame checked;
            rf_frame template_frame = {}; template_frame.size = CONTROLLER_STATE_BYTES;
            memcpy(template_frame.bytes, r.state, CONTROLLER_STATE_BYTES);
            if (result == ESP_OK && (!rf_set_target(&checked, &template_frame, RF_OFF, 0) ||
                memcmp(r.state + 6, d->pair.radiator, 4))) result = ESP_ERR_INVALID_ARG;
        }
        if (result == ESP_OK) {
            mutation_pending = true;
            result = queue_request(r);
            if (result != ESP_OK) mutation_pending = false;
            else {
                status.mutation_error = 0; status.mutation_pending = true;
                if (action == STATE_SAVE) d->state_save_result = RADIO_STORE_PENDING;
                if (action == STATE_CLEAR) d->state_clear_result = RADIO_STORE_PENDING;
                if (version) {
                    status.adoption_id = d->id;
                    memcpy(status.adoption_version, version, sizeof(status.adoption_version));
                    status.adoption_result = RADIO_STORE_PENDING; status.adoption_error = 0;
                }
            }
        }
    }
    unlock_status(); return result;
}
extern "C" esp_err_t radio_state_save(uint32_t id, const rf_frame *f) { return enqueue_mutation(id, STATE_SAVE, f, nullptr); }
extern "C" esp_err_t radio_state_clear(uint32_t id) { return enqueue_mutation(id, STATE_CLEAR, nullptr, nullptr); }
extern "C" esp_err_t radio_device_rename(uint32_t id, const char *name) { return name ? enqueue_mutation(id, RENAME, nullptr, name) : ESP_ERR_INVALID_ARG; }
extern "C" esp_err_t radio_device_remove(uint32_t id) { return enqueue_mutation(id, REMOVE, nullptr, nullptr); }

extern "C" esp_err_t radio_candidate_adopt(uint32_t id, const uint8_t radiator[4], const char *version, bool replace) {
    if (!radiator || !version || strlen(version) != 32) return ESP_ERR_INVALID_ARG;
    return enqueue_mutation(id, STATE_SAVE, nullptr, nullptr, radiator, version, replace);
}

static void save_pair(const rf_pairing &pair, bool &saved, int &error) {
    // Defense in depth: never persist paired control over a monitored address.
    for (size_t i=0;i<monitors.count;++i)
        if (!memcmp(monitors.records[i].address,pair.radiator,4)) { error=ESP_ERR_INVALID_STATE; return; }
    for (size_t i = 0; i < registry.count; ++i)
        if (!memcmp(registry.records[i].pair.radiator, pair.radiator, 4)) { error = ESP_ERR_INVALID_STATE; return; }
    if (registry.count >= CLEVERBOX_RADIATOR_CAPACITY) { error = ESP_ERR_INVALID_STATE; return; }
    radiator_record d = {};
    d.id = status.pairing_id; memcpy(d.name, "Radiator", 9);
    memcpy(d.pair.controller, pair.controller, 4); memcpy(d.pair.radiator, pair.radiator, 4);
    error = device_registry_save_record(&registry, &d, 0); saved = error == ESP_OK;
    if (saved) {
        lock_status();
        registry.records[registry.count++] = d; registry.next_id = d.id + 1;
        unlock_status();
    }
}

static bool receive_count(uint8_t &count)
{
    // TI SWRZ020E: RXBYTES can be torn while the radio updates it.
    // Bound the consecutive-read check; try again next tick if it stays unstable.
    uint8_t previous = module.SPIreadRegister(RADIOLIB_CC1101_REG_RXBYTES | 0x40);
    for (int i = 0; i < 8; ++i) {
        count = module.SPIreadRegister(RADIOLIB_CC1101_REG_RXBYTES | 0x40);
        if (count == previous) return true;
        previous = count;
    }
    return false;
}

// Reapply the exact established profile. No protocol machines or TX here.
static int initialize_radio() {
    hal.begin_operation(1200);
    // Working receive assumptions from the RF spec, not a calibrated TX profile.
    int result = radio.begin(868.3, 1000.0 / 27.0, 17.0, 101.6, 0, 32);
    int version = result == RADIOLIB_ERR_NONE ? radio.getChipVersion() : -1;
    lock_status(); status.chip_version = version; unlock_status();
    // Preamble length is not established; do not require a 28-bit preamble.
    if (result == RADIOLIB_ERR_NONE && !hal.error()) result = radio.setPreambleLength(32, 0);
    if (result == RADIOLIB_ERR_NONE && !hal.error()) result = radio.setEncoding(RADIOLIB_ENCODING_NRZ);
    if (result == RADIOLIB_ERR_NONE && !hal.error()) result = radio.setSyncWord(0xd3, 0x91);
    // CC1101 30/32-bit sync detection for the repeated D391 D391 word.
    if (result == RADIOLIB_ERR_NONE && !hal.error())
        result = module.SPIsetRegValue(RADIOLIB_CC1101_REG_MDMCFG2, 3, 2, 0);
    if (result == RADIOLIB_ERR_NONE && !hal.error()) result = radio.setCrcFiltering(false);
    if (result == RADIOLIB_ERR_NONE && !hal.error()) result = radio.disableAddressFiltering();
    // Infinite packet mode retains L and both software CRCs in the FIFO.
    // Variable-length hardware mode would stop before the outer CRC.
    if (result == RADIOLIB_ERR_NONE && !hal.error())
        result = module.SPIsetRegValue(RADIOLIB_CC1101_REG_PKTCTRL0, 2, 1, 0);
    if (result == RADIOLIB_ERR_NONE && !hal.error())
        result = module.SPIsetRegValue(RADIOLIB_CC1101_REG_PKTCTRL1, 0, 2, 2);
    if (result == RADIOLIB_ERR_NONE && !hal.error()) result = radio.startReceive();
    hal.pinMode(RADIO_GDO2, hal.GpioModeInput);
    return hal.error() ? hal.error() : result;
}

// One worker iteration is shared by FreeRTOS and the host's controllable clock.
struct radio_worker {
    rf_frame frame = {};
    int64_t started = -1;
    rf_pairing pair = status.pairing;
    bool saved = status.pairing_saved;
    int pair_error = status.pairing_error;
    rf_control controls[CLEVERBOX_RADIATOR_CAPACITY] = {};
    int control_errors[CLEVERBOX_RADIATOR_CAPACITY] = {};
    size_t next_device = 0; // Round-robin choice among frames ready for TX.
    // No pointers into the compactable registry or stack-owned outgoing frame.
    struct physical_tx {
        uint8_t bytes[64] = {};
        size_t size = 0;
        uint32_t id = 0;
        bool active = false, pairing = false, uncertain = false;
        int64_t deadline_us = 0;
    } tx;
    static_assert(sizeof(physical_tx) == 88, "Review retained TX resource budget");
    static constexpr int64_t TX_WAIT_US = 100000;
    void tick() {
        const int64_t now = esp_timer_get_time()/1000;
        // Once physically started, the ready-to-send deadline no longer applies.
        // The physical deadline bounds that owner; every other command still ages.
        if (!tx.active || !tx.pairing) rf_pair_tick(&pair, now);
        for (size_t i=0; i<registry.count; ++i)
            if (!tx.active || tx.pairing || registry.records[i].id != tx.id)
                rf_control_tick(&controls[i], now);
    }
    void resolve_tx(int result, radio_failure_stage stage) {
        // Exactly one cleanup/restoration, also after a failed start/timeout.
        // A new operation may retry a pending SPI completion, never reuse it.
        hal.begin_operation(1000);
        int rx = radio.standby();
        // finishTransmit normally sets GDO2 high-Z. Restore that mapping even
        // after start/timeout/finalizer errors before returning to infinite RX.
        if (!rx && !hal.error()) rx = module.SPIsetRegValue(RADIOLIB_CC1101_REG_IOCFG2, RADIOLIB_CC1101_GDOX_HIGH_Z, 5, 0);
        if (!rx && !hal.error()) rx = module.SPIsetRegValue(RADIOLIB_CC1101_REG_PKTCTRL0, 2, 1, 0);
        if (!rx && !hal.error()) rx = radio.startReceive();
        if (hal.error()) rx = hal.error();
        const int outcome = (result || rx) && tx.uncertain ? RADIO_ERR_DELIVERY_UNCERTAIN : result;
        if (tx.pairing) {
            pair_error = outcome;
            rf_pair_sent(&pair, esp_timer_get_time()/1000, result == 0);
        } else {
            radiator_record *d = find_device(tx.id);
            // Queue processing/compaction is suspended while tx.active. Admission
            // cannot replace this reserved command. Resolve by stable identity.
            if (d) {
                size_t i = d - registry.records;
                next_device = (i+1) % registry.count;
                control_errors[i] = outcome;
                if (!tx.uncertain && result) controls[i].status.phase = RF_CONTROL_ERROR;
                else rf_control_sent(&controls[i], esp_timer_get_time()/1000, result == 0);
            }
        }
        // Retain the original failure if cleanup also fails.
        if (result || rx) fail(result ? result : rx, result ? stage : RADIO_FAILURE_RX_RESTORE);
        else publish();
        lock_status(); tx_in_progress = false; unlock_status();
        tx = {};
    }
    void start_tx(const rf_frame &outgoing, bool pairing_tx, uint32_t id) {
        lock_status();
        int refusal = !rf_valid(&outgoing) || outgoing.size > 64 ? ESP_ERR_INVALID_ARG :
                      !rf_allowed() ? RADIO_ERR_LISTEN_ONLY : 0;
        if (!refusal) tx_in_progress = true;
        unlock_status();
        if (refusal) {
            if (pairing_tx) { pair_error = refusal; rf_pair_sent(&pair, esp_timer_get_time()/1000, false); }
            else if (auto *d = find_device(id)) {
                size_t i = d - registry.records;
                controls[i].status.phase = RF_CONTROL_ERROR; control_errors[i] = refusal;
            }
            publish(); return;
        }
        memcpy(tx.bytes, outgoing.bytes, outgoing.size); tx.size = outgoing.size; tx.id = id; tx.pairing = pairing_tx; tx.active = true;
        hal.begin_operation(1000);
        int result = radio.standby();
        if (!result && !hal.error()) result = radio.fixedPacketLengthMode(tx.size);
        radio_failure_stage stage = RADIO_FAILURE_TX;
        if (!result && !hal.error()) {
            stage = RADIO_FAILURE_TX_START;
            result = radio.startTransmit(tx.bytes, tx.size);
            // The HAL records submission of the STX strobe, including uncertain
            // bus completion. Earlier suppressed/failed setup cannot send bytes.
            tx.uncertain = hal.tx_started();
        }
        if (hal.error()) result = hal.error();
        if (result) { resolve_tx(result, stage); return; }
        tx.deadline_us = esp_timer_get_time() + TX_WAIT_US;
        publish(); // No completion polling until the next owner tick.
    }
    void progress_tx() {
        if (esp_timer_get_time() >= tx.deadline_us) {
            resolve_tx(RADIOLIB_ERR_TX_TIMEOUT, RADIO_FAILURE_TX_COMPLETE); return;
        }
        // Constant bounded samples, no wait for a signal transition. GDO2 is
        // sync-sent/packet-end; a whole pulse may fall between owner ticks.
        hal.begin_operation(250);
        const bool high = hal.digitalRead(RADIO_GDO2) != 0;
        uint8_t previous = module.SPIreadRegister(RADIOLIB_CC1101_REG_TXBYTES | 0x40);
        uint8_t count = module.SPIreadRegister(RADIOLIB_CC1101_REG_TXBYTES | 0x40);
        uint8_t marc = module.SPIreadRegister(RADIOLIB_CC1101_REG_MARCSTATE | 0x40) & 0x1f;
        int result = hal.error();
        if (!result && ((previous | count) & 0x80 || marc == RADIOLIB_CC1101_MARC_STATE_TXFIFO_UNDERFLOW)) result = RADIO_ERR_TX_UNDERFLOW;
        if (!result && esp_timer_get_time() >= tx.deadline_us) result = RADIOLIB_ERR_TX_TIMEOUT;
        if (result) { resolve_tx(result, RADIO_FAILURE_TX_COMPLETE); return; }
        // Low alone could mean not yet started or broken wiring. Require IDLE
        // and two matching empty FIFO reads before calling the real finalizer.
        if (!high && marc == RADIOLIB_CC1101_MARC_STATE_IDLE && count == 0 && previous == count) {
            result = radio.finishTransmit();
            if (hal.error()) result = hal.error();
            resolve_tx(result, RADIO_FAILURE_TX_COMPLETE);
        } else publish();
    }
    void publish() {
        lock_status();
        status.pairing = pair; status.pairing_saved = saved; status.pairing_error = pair_error;
        for (size_t i = 0; i < registry.count; ++i) {
            radiator_record &d = registry.records[i];
            if (d.command_reserved && !d.command_pending) {
                d.control = controls[i].status; d.control_error = control_errors[i];
                if (d.command_cookie) {
                    d.result_cookie = d.command_cookie; d.tracked_result = d.control; d.tracked_error = d.control_error;
                }
                // Do not release a newly queued reservation before its worker handoff.
                if (!rf_control_active(&d.control))
                    d.command_reserved = false;
            }
        }
        if (pair_reserved && !exchange_pending &&
            (saved || (!rf_pair_active(&pair) && pair.phase != RF_PAIR_COMPLETE))) {
            exchange_id = 0; pair_reserved = false;
        }
        status.exchange_id = exchange_id; status.device_count = registry.count;
        unlock_status();
    }
    void fail(int error, radio_failure_stage stage = RADIO_FAILURE_RECEIVE) {
        // Admission closes before reservations/queue entries are touched. Local
        // writes remain queued: only their actual store result can complete them.
        lock_status();
        status.ready = false; status.health = RADIO_RECOVERING;
        status.error = status.failure_code = error; status.failure_stage = stage;
        status.recovery_attempt = 0; status.retry_at_ms = esp_timer_get_time()/1000 + 1000;
        for (size_t i = 0; i < registry.count; ++i) {
            radiator_record &d = registry.records[i];
            if (!d.command_reserved) continue;
            if (!d.command_pending) d.control = controls[i].status;
            if (!d.command_pending && (d.control.phase == RF_CONTROL_MATCHED || d.control.phase == RF_CONTROL_TIMEOUT)) {
                // A validated readback/deadline already completed this command
                // before a later RX restoration fault. Keep that actual outcome.
                d.control_error = control_errors[i];
                if (d.command_cookie) { d.tracked_result = d.control; d.tracked_error = d.control_error; }
                d.command_pending = d.command_reserved = false;
                continue;
            }
            int outcome = (!d.command_pending && control_errors[i] == RADIO_ERR_DELIVERY_UNCERTAIN) || d.control.attempts
                ? RADIO_ERR_DELIVERY_UNCERTAIN : error;
            controls[i].status.phase = d.control.phase = RF_CONTROL_ERROR;
            control_errors[i] = d.control_error = outcome;
            if (d.command_cookie) { d.tracked_result = d.control; d.tracked_error = outcome; }
            d.command_pending = d.command_reserved = false;
        }
        if (pair_reserved) {
            // A completed, successfully persisted inclusion stays saved even
            // when restoring RX after its acknowledgement fails.
            if (exchange_pending || !saved) {
                if (exchange_pending) saved = false; // Previous inclusion's save is not this queued attempt.
                bool uncertain = pair_error == RADIO_ERR_DELIVERY_UNCERTAIN ||
                    (!exchange_pending && pair.phase >= RF_PAIR_WAIT_REPLY && pair.phase <= RF_PAIR_WAIT_ACK);
                pair.phase = status.pairing.phase = RF_PAIR_ERROR;
                pair_error = status.pairing_error = uncertain ? RADIO_ERR_DELIVERY_UNCERTAIN : error;
            }
            status.pairing = pair; status.pairing_saved = saved;
            pair_reserved = exchange_pending = false;
            exchange_id = status.exchange_id = 0;
        }
        // Bounded in-place filtering, preserving the order of accepted local
        // writes. No old RF handoff can be mistaken for a newly admitted one.
        unsigned count = uxQueueMessagesWaiting(radio_requests);
        radio_request r;
        for (unsigned i=0; i<count; ++i) {
            if (xQueueReceive(radio_requests,&r,0)!=pdPASS) break;
            if (r.action > CONTROL_START) (void)xQueueSend(radio_requests,&r,0);
        }
        unlock_status();
        frame = {}; started = -1;
        ESP_LOGE("radio", "Fault stage=%s code=%d; recovery scheduled", radio_failure_name(stage), error);
    }
    void recover() {
        lock_status();
        if (status.retry_pending) {
            status.retry_pending = false; status.health = RADIO_RECOVERING;
            status.recovery_attempt = 0; status.retry_at_ms = esp_timer_get_time()/1000;
        }
        bool attempt = (status.health == RADIO_INITIALIZING || status.health == RADIO_RECOVERING) &&
            esp_timer_get_time()/1000 >= status.retry_at_ms;
        if (attempt) { ++status.recovery_attempt; increment(status.recovery_attempts); }
        unlock_status();
        if (!attempt) return;
        int result = initialize_radio();
        lock_status();
        status.error = result;
        if (result == RADIOLIB_ERR_NONE) {
            if (status.failure_stage != RADIO_FAILURE_NONE) increment(status.recovery_successes);
            status.health = RADIO_READY; status.ready = true; status.retry_at_ms = 0;
        } else {
            status.failure_stage = RADIO_FAILURE_INITIALIZE; status.failure_code = result;
            if (status.recovery_attempt == RADIO_RECOVERY_ATTEMPTS) {
                status.health = RADIO_FAULTED; status.retry_at_ms = 0;
            } else status.retry_at_ms = esp_timer_get_time()/1000 +
                (1000LL << (status.recovery_attempt - 1)); // 1, 2, 4 seconds; never unbounded
        }
        unlock_status();
        frame = {}; started = -1;
        ESP_LOGI("radio", "Setup attempt=%u result=%d", status.recovery_attempt, result);
    }
    void step() {
        if (tx.active) {
            tick(); progress_tx(); return;
        }
        radio_request request;
        if (xQueueReceive(radio_requests, &request, 0) == pdPASS) {
            if (request.action == MODE_SET) {
                int result = device_mode_save(request.enabled);
                lock_status();
                // Failed persistence always closes RF, including a failed disable
                // that might have reached NVS. Never report a durable success.
                status.control_enabled = result == ESP_OK && request.enabled;
                status.mode_error = result;
                status.mode_persistence_uncertain = result != ESP_OK;
                status.mode_save_result = result == ESP_OK ? RADIO_STORE_OK : RADIO_STORE_FAILED;
                status.mode_pending = false;
                unlock_status();
            } else if (request.action == MONITOR_SET || request.action == MONITOR_REMOVE) {
                // Prospective list is bounded BSS, never copied onto task stack.
                static monitor_registry changed;
                changed=monitors; size_t i=0;
                while (i<changed.count && memcmp(changed.records[i].address,request.candidate,4)) ++i;
                bool remove=request.action==MONITOR_REMOVE;
                if (remove) {
                    for (size_t j=i+1;j<changed.count;++j) changed.records[j-1]=changed.records[j];
                    --changed.count;
                } else {
                    if (i==changed.count) ++changed.count;
                    memcpy(changed.records[i].address,request.candidate,4); strcpy(changed.records[i].name,request.name);
                }
                int e=device_monitors_save(&changed);
                lock_status();
                if (e==ESP_OK) {
                    if (remove) {
                        for (size_t j=i+1;j<monitors.count;++j) monitored[j-1]=monitored[j];
                        monitored[changed.count]={};
                    } else {
                        if (i==monitors.count) {
                            monitored[i]={};
                            for (size_t j=0;j<status.observation_count;++j)
                                if (!memcmp(observations[j].report.source,request.candidate,4)) {
                                    monitored[i].available=true; monitored[i].observation=observations[j]; break;
                                }
                        }
                        monitored[i].selection=changed.records[i];
                    }
                    monitors=changed;
                }
                status.monitor_result=e==ESP_OK ? RADIO_STORE_OK : RADIO_STORE_FAILED;
                status.monitor_uncertain=e!=ESP_OK;
                // An uncertain list freezes mutations until reboot. The old
                // RAM list is authoritative this boot; journal retains its topics.
                status.monitor_error=e; mutation_pending=status.monitor_pending=false;
                unlock_status();
            } else if (request.action >= STATE_SAVE) {
                radiator_record *d = find_device(request.id);
                int result = ESP_ERR_NOT_FOUND;
                if (d) {
                    radiator_record changed = *d;
                    if (request.action == RENAME) memcpy(changed.name, request.name, sizeof(changed.name));
                    else if (request.action != REMOVE) {
                        changed.has_saved_template = request.action == STATE_SAVE;
                        memset(changed.saved_template, 0, sizeof(changed.saved_template));
                        if (changed.has_saved_template) memcpy(changed.saved_template, request.state, CONTROLLER_STATE_BYTES);
                    }
                    result = device_registry_save_record(&registry, request.action == REMOVE ? nullptr : &changed,
                                                        request.action == REMOVE ? d->id : 0);
                    lock_status();
                    if (result == ESP_OK) {
                        if (request.action == REMOVE) {
                            size_t index = d - registry.records;
                            for (size_t i = index + 1; i < registry.count; ++i) registry.records[i - 1] = registry.records[i];
                            registry.records[--registry.count] = {};
                            for (size_t i = index; i < registry.count; ++i) { controls[i] = controls[i + 1]; control_errors[i] = control_errors[i + 1]; }
                            controls[registry.count] = {}; control_errors[registry.count] = 0;
                            next_device = 0;
                        } else {
                            *d = changed; d->state_load_error = 0;
                            if (request.action == STATE_CLEAR || request.adoption_version[0]) {
                                d->has_control_template = false; memset(d->control_template, 0, sizeof(d->control_template));
                                d->control_template_time_us = 0;
                            }
                        }
                    }
                    if (request.action == STATE_SAVE || request.action == STATE_CLEAR) {
                        d->state_store_error = result;
                        (request.action == STATE_SAVE ? d->state_save_result : d->state_clear_result) =
                            result == ESP_OK ? RADIO_STORE_OK : RADIO_STORE_FAILED;
                    }
                    if (result == ESP_OK) status.registry_error = 0;
                    unlock_status();
                }
                lock_status();
                if (request.adoption_version[0]) {
                    status.adoption_result = result == ESP_OK ? RADIO_STORE_OK : RADIO_STORE_FAILED;
                    status.adoption_error = result;
                }
                status.mutation_error = result; mutation_pending = status.mutation_pending = false;
                unlock_status();
            } else if (request.action == CONTROL_START) {
                radiator_record *d = find_device(request.id);
                if (d) {
                    size_t index = d - registry.records;
                    rf_control_request command = {}; command.state.size = CONTROLLER_STATE_BYTES;
                    memcpy(command.state.bytes, request.state, CONTROLLER_STATE_BYTES);
                    command.mode = request.mode; command.centi_c = request.centi_c; command.clock_seconds = request.clock_seconds;
                    bool started = rf_control_start(&controls[index], &command, d->pair.controller, d->pair.radiator,
                                                    request.admitted_ms);
                    lock_status();
                    d->command_pending = false;
                    if (started) control_errors[index] = 0;
                    else { d->command_reserved = false; d->control = {}; }
                    unlock_status();
                }
            } else if (request.action == PAIR_CANCEL && request.id == exchange_id && pair_reserved) {
                rf_pair_cancel(&pair);
                if (pair.phase == RF_PAIR_COMPLETE && !saved) pair = {};
            } else if (request.action == PAIR_SAVE && pair.phase == RF_PAIR_COMPLETE && !saved)
                save_pair(pair, saved, pair_error);
            else if (request.action == PAIR_START) {
                saved = false; pair_error = 0; pair = {};
                lock_status(); exchange_pending = false; unlock_status();
                rf_pair_start(&pair, request.controller, request.candidate, request.admitted_ms);
            }
        }
        tick();
        if (!radio_transport_status().ready) { recover(); return; }
        hal.begin_operation(250);
        service_receive();
        if (!radio_transport_status().ready) return;
        if (hal.error()) { fail(hal.error()); return; }
        tick();
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (started >= 0 || frame.size) { publish(); return; }
        rf_frame outgoing = {};
        bool pairing_tx = rf_pair_next(&pair, now_ms, &outgoing);
        bool control_tx = false;
        size_t chosen = 0;
        if (!pairing_tx) for (size_t offset = 0; offset < registry.count; ++offset) {
            size_t i = (next_device + offset) % registry.count;
            if (rf_control_next(&controls[i], now_ms, &outgoing)) {
                chosen = i; control_tx = true; break;
            }
        }
        // next() is read-only. A deferral retains the frame in its machine,
        // without a sent callback, attempt increment or round-robin advancement.
        bool idle = (pairing_tx || control_tx) && receive_idle();
        if (hal.error()) { fail(hal.error()); return; }
        if (idle) {
            // The only change since read-only selection can be expiry. Recheck
            // that deadline after SPI sampling, without rebuilding the frame and
            // widening the final FIFO-check-to-standby window.
            now_ms = esp_timer_get_time() / 1000;
            rf_pair_tick(&pair, now_ms);
            if (control_tx) rf_control_tick(&controls[chosen], now_ms);
            bool still_ready = pairing_tx ? rf_pair_active(&pair) :
                                            rf_control_active(&controls[chosen].status);
            if (!still_ready) { publish(); return; }
            start_tx(outgoing, pairing_tx, control_tx ? registry.records[chosen].id : exchange_id);
            return;
        }
        publish();
    }
    // All FIFO/radio access stays in this owner. Unknown counts are busy, even
    // when no bytes have yet reached the software buffer. No spin-until-idle.
    bool receive_idle() {
        if (started >= 0 || frame.size) return false;
        uint8_t count = 0;
        if (!receive_count(count) || count) {
            started = esp_timer_get_time();
            return false;
        }
        // Half-duplex limitation: bytes can still arrive after this check.
        return true;
    }
    void service_receive() {
        uint8_t count = 0;
        bool stable = receive_count(count);
        if (hal.error()) { fail(hal.error()); return; }
        if ((!stable || count) && started < 0) started = esp_timer_get_time();
        bool restart = stable && (count & 0x80) != 0;
        bool receive_error = restart;
        int result = RADIOLIB_ERR_NONE;
        count &= 0x7f;
        if (stable && !restart && count) {
            if (count > 64) {
                restart = receive_error = true;
            } else {
                // Never empty a live FIFO: TI's RX FIFO erratum can duplicate
                // the last byte when it is read as a new RF byte arrives.
                if (!frame.size && count > 1) {
                    frame.bytes[0] = module.SPIreadRegister(RADIOLIB_CC1101_REG_FIFO);
                    frame.size = 1;
                    --count;
                }
                size_t expected = (size_t)frame.bytes[0] + 3;
                if (frame.size && frame.bytes[0] >= 11) {
                    size_t remaining = expected - frame.size;
                    bool complete = count >= remaining;
                    size_t take = complete ? remaining : (count > 1 ? count - 1 : 0);
                    // Infinite mode has no hardware packet end. Stop RX before
                    // the final drain, and leave any tail/noise for the flush.
                    if (complete) result = radio.standby();
                    if (result == RADIOLIB_ERR_NONE && take) {
                        module.SPIreadRegisterBurst(RADIOLIB_CC1101_REG_FIFO | 0x40, take,
                                                   frame.bytes + frame.size);
                        frame.size += take;
                    }
                    if (result != RADIOLIB_ERR_NONE) restart = receive_error = true;
                }
                if (hal.error()) { fail(hal.error()); return; }
                if (result == RADIOLIB_ERR_NONE && frame.size &&
                    (frame.bytes[0] < 11 || frame.size == expected)) {
                    bool valid = rf_valid(&frame);
                    rf_report report = {};
                    bool decoded = valid && rf_decode_report(&frame, &report);
                    int64_t now = esp_timer_get_time();
                    rf_pair_phase before = pair.phase;
                    if (valid) rf_pair_receive(&pair, now / 1000, &frame);
                    if (before == RF_PAIR_WAIT_REPLY && pair.phase == RF_PAIR_BIND) {
                        for (size_t i = 0; i < registry.count; ++i)
                            if (!memcmp(registry.records[i].pair.radiator, pair.radiator, 4)) {
                                pair.phase = RF_PAIR_ERROR; pair_error = ESP_ERR_INVALID_STATE;
                                lock_status(); status.pairing_duplicate = true; unlock_status();
                            }
                    }
                    if (before == RF_PAIR_WAIT_REPLY && pair.phase == RF_PAIR_BIND) {
                        for (size_t i=0;i<monitors.count;++i)
                            if (!memcmp(monitors.records[i].address,pair.radiator,4)) {
                                pair.phase=RF_PAIR_ERROR; pair_error=RADIO_ERR_MONITORED;
                            }
                    }
                    if (before != RF_PAIR_COMPLETE && pair.phase == RF_PAIR_COMPLETE)
                        save_pair(pair, saved, pair_error);
                    // Receive ownership is determined by both addresses, never UI selection.
                    rf_frame checked_state = {};
                    bool state_frame = valid && rf_set_target(&checked_state, &frame, RF_OFF, 0);
                    lock_status();
                    capture_passive(frame, decoded ? &report : nullptr, state_frame, now);
                    for (size_t i = 0; i < registry.count; ++i) {
                        radiator_record &d = registry.records[i];
                        if (state_frame && !memcmp(frame.bytes + 1, d.pair.controller, 4) &&
                            !memcmp(frame.bytes + 6, d.pair.radiator, 4)) {
                            d.has_control_template = true;
                            memcpy(d.control_template, frame.bytes, CONTROLLER_STATE_BYTES);
                            d.control_template_time_us = now;
                        }
                        if (decoded && !memcmp(report.source, d.pair.radiator, 4) &&
                            !memcmp(report.destination, d.pair.controller, 4)) {
                            d.has_paired_report = true; d.paired_report = report; d.paired_report_time_us = now;
                            if (d.command_reserved && !d.command_pending) {
                                rf_control_receive(&controls[i], now / 1000, &frame);
                                if (controls[i].status.phase == RF_CONTROL_MATCHED) control_errors[i] = 0;
                            } else if ((d.control.phase == RF_CONTROL_TIMEOUT || d.control.phase == RF_CONTROL_ERROR) &&
                                       rf_control_reconcile(&d.control, now / 1000, &frame)) {
                                d.control_error = 0;
                                if (d.command_cookie) { d.tracked_result = d.control; d.tracked_error = 0; }
                            }
                        }
                    }
                    if (valid) {
                        increment(status.valid_frames);
                        status.has_frame = true;
                        status.last_frame = frame;
                        status.frame_time_us = now;
                        if (decoded) {
                            status.has_report = true;
                            status.last_report = report;
                            status.report_time_us = now;
                        }
                    } else increment(status.invalid_frames);
                    unlock_status();
                    restart = true;
                }
            }
        }
        // A maximum 258-byte envelope takes about 56 ms at this bitrate.
        if (started >= 0 && esp_timer_get_time() - started > 100000)
            restart = receive_error = true;
        if (restart) {
            if (result == RADIOLIB_ERR_NONE)
                result = radio.startReceive(); // standby, flush, re-enter RX
            if (hal.error()) result = hal.error();
            lock_status();
            if (receive_error) increment(status.receive_errors);
            status.error = result;
            status.ready = result == RADIOLIB_ERR_NONE;
            unlock_status();
            if (result != RADIOLIB_ERR_NONE) {
                ESP_LOGE("radio", "Receive failed: %d", result);
                fail(result); return;
            }
            frame = {};
            started = -1;
        }
    }
};

static void receive_task(void *)
{
    // Capacity-dependent machines reside in BSS, not the fixed-size task stack.
    static radio_worker worker;
    for (;;) {
        worker.step();
        vTaskDelay(1);
    }
}

extern "C" void radio_transport_start(void)
{
    status_lock = xSemaphoreCreateMutex();
    if (!status_lock) {
        status.health = RADIO_FAULTED; status.error = status.failure_code = ESP_ERR_NO_MEM;
        status.failure_stage = RADIO_FAILURE_RESOURCES; return;
    }
    status.capacity = CLEVERBOX_RADIATOR_CAPACITY;
    observations[0] = {}; candidates[0] = {};
    status.observation_count = status.candidate_count = 0;
    capture_sequence = 0;
    capture_boot = ((uint64_t)esp_random() << 32) | esp_random();
    bool enabled = false;
    int mode_result = device_mode_start(&enabled);
    status.control_enabled = mode_result == ESP_OK && enabled;
    status.mode_error = mode_result;
    status.mode_persistence_uncertain = mode_result != ESP_OK;
    status.mode_save_result = mode_result == ESP_OK ? RADIO_STORE_OK : RADIO_STORE_FAILED;
    int load_result = device_registry_load(&registry);
    status.monitor_error=device_monitors_load(&monitors);
    if (status.monitor_error) { monitors.count=0; status.monitor_uncertain=true; }
    for (size_t i=0;i<monitors.count;++i) { monitored[i]={}; monitored[i].selection=monitors.records[i]; }
    status.registry_error = load_result;
    status.device_count = registry.count;
    if (registry.count == 1) {
        status.pairing.phase = RF_PAIR_COMPLETE; status.pairing_saved = true;
        status.pairing_id = registry.records[0].id;
        memcpy(status.pairing.controller, registry.records[0].pair.controller, 4);
        memcpy(status.pairing.radiator, registry.records[0].pair.radiator, 4);
    }
    if (load_result != ESP_OK) status.pairing_error = load_result;
    radio_requests = xQueueCreate(CLEVERBOX_RADIATOR_CAPACITY + 1, sizeof(radio_request));
    status.health = RADIO_INITIALIZING;
    status.chip_version = -1;
    if (!radio_requests) { status.health = RADIO_FAULTED; status.error = ESP_ERR_NO_MEM;
        status.failure_stage = RADIO_FAILURE_RESOURCES; status.failure_code = status.error; return; }
    status.worker_running = true;
    if (xTaskCreate(receive_task, "radio_rx", 6144, nullptr, 5, nullptr) != pdPASS) {
        status.worker_running = false; status.health = RADIO_FAULTED;
        status.error = status.failure_code = ESP_ERR_NO_MEM; status.failure_stage = RADIO_FAILURE_RESOURCES;
    }
}
