// SPDX-License-Identifier: MIT
#ifndef RADIATOR_PROTOCOL_H
#define RADIATOR_PROTOCOL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RF_MAX_FRAME 258
typedef struct { uint8_t bytes[RF_MAX_FRAME]; size_t size; } rf_frame;
typedef enum { RF_OFFER, RF_BIND, RF_POLL } rf_command;
typedef enum { RF_COMFORT = 0, RF_OFF = 1, RF_FROST = 2, RF_ECO = 3 } rf_mode;

bool rf_valid(const rf_frame *frame);
bool rf_short(rf_frame *out, const uint8_t source[4], const uint8_t dest[4], rf_command command);
// Preserve the complete controller-state template except target/mode and CRCs.
// Temperature is centidegrees Celsius; Off always uses raw 320.
bool rf_set_target(rf_frame *out, const rf_frame *state, rf_mode mode, int centi_c);
bool rf_parse_hex(const char *hex, uint8_t *out, size_t capacity, size_t *size);
void rf_to_hex(const rf_frame *frame, char out[RF_MAX_FRAME * 2 + 1]);
typedef struct {
    uint8_t source[4], destination[4];
    bool temperature_available, target_available;
    int temperature_centi_c, target_centi_c;
    uint8_t mode; // Preserve unknown mode values.
} rf_report;
// Decode only the documented radiator-report shape, after validating both CRCs.
bool rf_decode_report(const rf_frame *frame, rf_report *report);
// Build a live state from a same-radiator template and a newly received report.
// Refresh addresses, measured temperature, E1, zero DA and local clock; keep opaque fields.
bool rf_live_state(rf_frame *out, const rf_frame *state, const rf_frame *report,
                   const uint8_t controller[4], const uint8_t radiator[4],
                   rf_mode mode, int centi_c, unsigned clock_seconds);
#ifdef __cplusplus
}
#endif
#endif
