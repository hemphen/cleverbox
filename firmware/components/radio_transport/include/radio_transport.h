// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "radiator_protocol.h"
#include "radiator_pairing.h"
#include "esp_err.h"
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
    bool pairing_saved;
    int pairing_error;
} radio_status;
// Starts reception and loads saved pairing without transmitting. Failure leaves the HTTP interface available.
void radio_transport_start(void);
radio_status radio_transport_status(void);
// Enqueue one explicit action. Radio and storage work stay in the radio task.
esp_err_t radio_pair_start(const uint8_t controller[4], const uint8_t candidate[4]);
esp_err_t radio_pair_cancel(void);
esp_err_t radio_pair_save(void);
#ifdef __cplusplus
}
#endif
