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
typedef struct {
    bool ready;
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
esp_err_t radio_control_start(uint32_t id, const rf_control_request *request);
#ifdef __cplusplus
}
#endif
