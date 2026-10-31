// SPDX-License-Identifier: MIT
#pragma once
#include "esp_err.h"
#include "mode_store.h"
#include <stdint.h>
#include "radiator_protocol.h"
#include "radiator_control.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { uint8_t controller[4], radiator[4]; } paired_device;
// Capacity is a build setting, not an RF protocol limit. Names are UTF-8 bytes.
#ifndef CLEVERBOX_RADIATOR_CAPACITY
#define CLEVERBOX_RADIATOR_CAPACITY 8
#endif
#define RADIATOR_NAME_BYTES 32
#define CONTROLLER_STATE_BYTES 59
#define REGISTRY_HEADER_BYTES 7
#define REGISTRY_RECORD_BYTES (4 + RADIATOR_NAME_BYTES + 1 + 8 + 1 + CONTROLLER_STATE_BYTES)
#define REGISTRY_STORAGE_BYTES (REGISTRY_HEADER_BYTES + REGISTRY_RECORD_BYTES * CLEVERBOX_RADIATOR_CAPACITY)
#define DEVICE_STORE_ERR_CAPACITY 0x7c01
#if CLEVERBOX_RADIATOR_CAPACITY < 1 || CLEVERBOX_RADIATOR_CAPACITY > 65535
#error Invalid radiator capacity
#endif
typedef enum { RADIO_STORE_NONE, RADIO_STORE_PENDING, RADIO_STORE_OK, RADIO_STORE_FAILED } radio_store_result;
typedef struct {
    uint32_t id;
    char name[RADIATOR_NAME_BYTES + 1];
    paired_device pair;
    bool has_saved_template;
    uint8_t saved_template[CONTROLLER_STATE_BYTES];
    // Everything below is transient. The live machine is worker-owned.
    bool has_control_template, has_paired_report;
    uint8_t control_template[CONTROLLER_STATE_BYTES];
    int64_t control_template_time_us, paired_report_time_us;
    rf_report paired_report;
    rf_control_status control;
    bool command_reserved; // Admission through worker handoff and terminal publication.
    bool command_pending;
    // Optional transport correlation mailbox. Not persisted. Subsequent untracked
    // HTTP commands cannot erase a tracked command's final result before it is read.
    uint64_t command_cookie, result_cookie;
    rf_control_status tracked_result;
    int tracked_error;
    int control_error, state_load_error, state_store_error;
    radio_store_result state_save_result, state_clear_result;
} radiator_record;
typedef struct {
    uint32_t next_id;
    size_t count;
    radiator_record records[CLEVERBOX_RADIATOR_CAPACITY];
} radiator_registry;
// A present registry (even empty) is authoritative; legacy keys are never erased.
// Capacity/version/validation failures block mutation, never truncate records.
esp_err_t device_registry_load(radiator_registry *registry);
esp_err_t device_registry_save(const radiator_registry *registry);
// Save one prospective add/update/removal before publishing it to RAM.
esp_err_t device_registry_save_record(const radiator_registry *registry,
                                     const radiator_record *record, uint32_t remove_id);
#ifdef __cplusplus
}
#endif
