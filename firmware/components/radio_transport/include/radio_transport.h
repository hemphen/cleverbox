// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "radiator_protocol.h"
#include "radiator_pairing.h"
#include "radiator_control.h"
#include "device_store.h"
#ifdef __cplusplus
extern "C" {
#endif

// Errors returned by the request functions below, also used as result codes.
#define RADIO_ERR_NOT_READY 0x7301
#define RADIO_ERR_BUSY 0x7302
#define RADIO_ERR_TIME_UNAVAILABLE 0x7303
#define RADIO_ERR_FULL 0x7304
#define RADIO_ERR_DUPLICATE 0x7305
#define RADIO_ERR_NOT_PAIRED 0x7306
// Short snake_case name for an error code ("none" for ESP_OK).
const char *radio_error_name(int error);

#define RADIO_OBSERVATION_CAPACITY (2 * CLEVERBOX_RADIATOR_CAPACITY)
// Latest decoded report from any radiator address, whatever its controller.
typedef struct { rf_report report; int64_t time_ms; } radio_observation;

typedef struct {
    bool ready;
    int error;               // Last radio error; kept after a retry succeeds.
    const char *error_stage; // "none", "init", "receive" or "transmit".
    int chip_version;
    uint32_t valid_frames, invalid_frames, receive_errors, transmissions;
    bool has_frame;
    int64_t frame_ms;
    rf_frame last_frame;     // Latest CRC-valid frame from any address.
    rf_pairing pairing;      // Latest pairing attempt.
    uint32_t pairing_id;     // ID the paired radiator gets (or got).
    int pairing_error;
    int registry_error;      // Last registry load or save error.
    uint32_t registry_version; // Changes whenever a record is added, renamed or removed.
} radio_status;

// Loads the registry and starts the radio task. Nothing is transmitted.
void radio_transport_start(void);
radio_status radio_transport_status(void);
size_t radio_radiators(radiator_record *out, size_t capacity);
size_t radio_observations(radio_observation *out, size_t capacity);

// Reinitialize a radio that is not ready.
esp_err_t radio_retry(void);
// Explicit pairing: Cleverbox becomes `controller` of the radiator answering
// on `discovery`. *id is the ID the new record will get.
esp_err_t radio_pair_start(const uint8_t controller[4], const uint8_t discovery[4], uint32_t *id);
esp_err_t radio_pair_cancel(void);
// Queue a target/mode command for a paired radiator. Commands run one at a
// time in order; each radiator has at most one queued or running command.
esp_err_t radio_command(uint32_t id, rf_mode mode, int centi_c, uint64_t request_id);
esp_err_t radio_rename(uint32_t id, const char *name);
esp_err_t radio_remove(uint32_t id);
// Add a monitored (observe-only) record for a radiator address.
esp_err_t radio_monitor(const uint8_t radiator[4], const char *name, uint32_t *id);
#ifdef __cplusplus
}
#endif
