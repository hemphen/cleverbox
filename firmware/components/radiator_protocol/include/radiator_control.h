// SPDX-License-Identifier: MIT
#pragma once
#include "radiator_protocol.h"
#ifdef __cplusplus
extern "C" {
#endif
#define RF_CONTROL_WAIT_MS 150000
#define RF_CONTROL_MAX_ATTEMPTS 2
#define RF_REPORT_STALE_MS 300000

typedef enum {
    RF_CONTROL_IDLE, RF_CONTROL_POLL, RF_CONTROL_WAIT_REPORT, RF_CONTROL_SEND,
    RF_CONTROL_WAIT_READBACK, RF_CONTROL_MATCHED, RF_CONTROL_TIMEOUT, RF_CONTROL_ERROR
} rf_control_phase;
typedef struct {
    rf_frame state;
    rf_mode mode;
    int centi_c;
    unsigned clock_seconds; // Local time at request acceptance, supplied by the client.
} rf_control_request;
typedef struct {
    rf_control_phase phase;
    rf_mode mode;
    int centi_c;
    unsigned attempts;
    int64_t matched_ms;
} rf_control_status;
typedef struct {
    rf_control_status status;
    rf_control_request request;
    rf_frame report;
    uint8_t controller[4], radiator[4];
    int64_t started_ms, deadline_ms;
} rf_control;
bool rf_control_active(const rf_control_status *s);
bool rf_control_start(rf_control *c, const rf_control_request *request,
                      const uint8_t controller[4], const uint8_t radiator[4], int64_t now_ms);
void rf_control_tick(rf_control *c, int64_t now_ms);
bool rf_control_next(const rf_control *c, int64_t now_ms, rf_frame *out);
void rf_control_sent(rf_control *c, int64_t now_ms, bool success);
void rf_control_receive(rf_control *c, int64_t now_ms, const rf_frame *frame);
const char *rf_control_phase_name(rf_control_phase phase);
#ifdef __cplusplus
}
#endif
