// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "radiator_protocol.h"
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
} radio_status;
// Starts receive-only operation. Failure leaves the HTTP interface available.
void radio_transport_start(void);
radio_status radio_transport_status(void);
#ifdef __cplusplus
}
#endif
