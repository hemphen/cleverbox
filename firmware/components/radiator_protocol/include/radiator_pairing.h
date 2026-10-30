// SPDX-License-Identifier: MIT
#pragma once
#include "radiator_protocol.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {
    RF_PAIR_IDLE, RF_PAIR_OFFER, RF_PAIR_WAIT_REPLY, RF_PAIR_BIND,
    RF_PAIR_WAIT_ACK, RF_PAIR_COMPLETE, RF_PAIR_TIMEOUT, RF_PAIR_CANCELLED, RF_PAIR_ERROR
} rf_pair_phase;
typedef struct {
    rf_pair_phase phase;
    uint8_t controller[4], candidate[4], radiator[4];
    int64_t deadline_ms, send_at_ms;
} rf_pairing;
bool rf_pair_active(const rf_pairing *p);
bool rf_pair_start(rf_pairing *p, const uint8_t controller[4], const uint8_t candidate[4], int64_t now_ms);
void rf_pair_cancel(rf_pairing *p);
void rf_pair_tick(rf_pairing *p, int64_t now_ms);
bool rf_pair_next(const rf_pairing *p, int64_t now_ms, rf_frame *out);
void rf_pair_sent(rf_pairing *p, int64_t now_ms, bool success);
void rf_pair_receive(rf_pairing *p, int64_t now_ms, const rf_frame *frame);
const char *rf_pair_phase_name(rf_pair_phase phase);
#ifdef __cplusplus
}
#endif
