// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "radiator_protocol.h"
#include "radiator_pairing.h"
#include "radiator_control.h"
#include "esp_err.h"
#include "device_store.h"
#ifdef __cplusplus
extern "C" {
#endif
// One finite batch: initial attempt, then 1/2/4 s delays. Runtime faults
// wait 1 s before the first attempt. A new fault starts a new batch.
#define RADIO_RECOVERY_ATTEMPTS 4
#define RADIO_ERR_DELIVERY_UNCERTAIN 0x730A
typedef enum { RADIO_INITIALIZING, RADIO_RECOVERING, RADIO_READY, RADIO_FAULTED } radio_health;
typedef enum { RADIO_FAILURE_NONE, RADIO_FAILURE_INITIALIZE, RADIO_FAILURE_RECEIVE,
               RADIO_FAILURE_TX, RADIO_FAILURE_RX_RESTORE, RADIO_FAILURE_RESOURCES } radio_failure_stage;
const char *radio_health_name(radio_health health);
const char *radio_failure_name(radio_failure_stage stage);
esp_err_t radio_retry(void);
#define RADIO_PASSIVE_CAPACITY (2 * CLEVERBOX_RADIATOR_CAPACITY)
#define RADIO_ERR_LISTEN_ONLY 0x7302
#define RADIO_ERR_MODE_BUSY 0x7303
#define RADIO_ERR_CANDIDATE_CHANGED 0x7304
#define RADIO_ERR_REPLACE_REQUIRED 0x7305
#define RADIO_ERR_MONITORED 0x7306
#define RADIO_ERR_MONITOR_CAPACITY 0x7307
#define RADIO_ERR_MONITOR_PAIRED 0x7308
#define RADIO_ERR_MONITOR_UNOBSERVED 0x7309
// Independent caches: reports can never evict captured templates.
typedef struct { rf_report report; int64_t time_us; } radio_observation;
typedef struct { monitor_selection selection; bool available; radio_observation observation; } radio_monitor;
esp_err_t radio_monitor_at(size_t index, radio_monitor *out);
esp_err_t radio_monitor_get(const uint8_t address[4], radio_monitor *out);
esp_err_t radio_monitor_change(const char *action, const uint8_t address[4], const char *name);
typedef struct {
    uint8_t state[CONTROLLER_STATE_BYTES];
    int64_t time_us;
    char version[33]; // boot nonce + never-reused capture sequence, hexadecimal
} radio_candidate;
typedef struct {
    bool control_enabled, mode_pending, mode_persistence_uncertain;
    int mode_error;
    radio_store_result mode_save_result;
    size_t observation_count, candidate_count;
    uint32_t observation_evictions, candidate_evictions;
    uint32_t adoption_id;
    char adoption_version[33];
    radio_store_result adoption_result;
    int adoption_error;
    int monitor_error;
    bool monitor_pending, monitor_uncertain;
    radio_store_result monitor_result;
    bool ready, worker_running, retry_pending;
    radio_health health;
    radio_failure_stage failure_stage;
    int failure_code;
    unsigned recovery_attempt;
    uint32_t recovery_attempts, recovery_successes;
    int64_t retry_at_ms; // monotonic; zero when ready/exhausted

    int error;
    int chip_version;
    uint32_t valid_frames, invalid_frames, receive_errors;
    bool has_frame, has_report;
    int64_t frame_time_us, report_time_us;
    rf_frame last_frame;
    rf_report last_report;
    rf_pairing pairing;
    bool pairing_saved, pairing_duplicate;
    int pairing_error;
    uint32_t pairing_id, exchange_id;
    size_t device_count, capacity;
    int registry_error, mutation_error;
    bool mutation_pending;
} radio_status;
esp_err_t radio_mode_set(bool control_enabled);
esp_err_t radio_observation_at(size_t index, radio_observation *out);
esp_err_t radio_candidate_at(size_t index, radio_candidate *out);
esp_err_t radio_candidate_adopt(uint32_t id, const uint8_t radiator[4], const char *version, bool replace);
// Starts reception and loads saved pairing without transmitting. Failure leaves the HTTP interface available.
void radio_transport_start(void);
radio_status radio_transport_status(void);
// id==0 resolves only a single configured radiator, never a browser selection.
esp_err_t radio_device_status(uint32_t id, radiator_record *out);
esp_err_t radio_device_at(size_t index, radiator_record *out);
esp_err_t radio_pair_start(const uint8_t controller[4], const uint8_t candidate[4], uint32_t *id);
esp_err_t radio_pair_cancel(uint32_t id);
esp_err_t radio_pair_save(uint32_t id);
esp_err_t radio_state_save(uint32_t id, const rf_frame *imported);
esp_err_t radio_state_clear(uint32_t id);
esp_err_t radio_device_rename(uint32_t id, const char *name);
esp_err_t radio_device_remove(uint32_t id);
// Normal admission snapshots device local time. No usable time queues nothing.
#define RADIO_ERR_TIME_UNAVAILABLE 0x7301
esp_err_t radio_control_start_device(uint32_t id, const rf_control_request *request);
// Same admission with an optional nonzero correlation cookie and last-result mailbox.
esp_err_t radio_control_start_device_tracked(uint32_t id, const rf_control_request *request, uint64_t cookie);
// Explicit time-of-day override retained for compatibility and bench tests.
esp_err_t radio_control_start(uint32_t id, const rf_control_request *request);
#ifdef __cplusplus
}
#endif
