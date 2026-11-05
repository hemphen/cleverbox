// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "radiator_protocol.h"
#include "radiator_control.h"
#ifdef __cplusplus
extern "C" {
#endif

// Small settings records in NVS namespace "cleverbox". Plain NVS calls: a
// failed write returns its error and the caller reports it.
esp_err_t store_load(const char *key, void *data, size_t *size);
esp_err_t store_save(const char *key, const void *data, size_t size);
esp_err_t store_erase(const char *key);

// Capacity is a build setting, not an RF protocol limit.
#ifndef CLEVERBOX_RADIATOR_CAPACITY
#define CLEVERBOX_RADIATOR_CAPACITY 16
#endif
#define RADIATOR_NAME_BYTES 32

// One record per radiator. A paired radiator has a nonzero controller address
// (Cleverbox is its controller). A monitored radiator has an all-zero
// controller and is only observed. ID, name and addresses persist; the rest is
// RAM-only and cleared at boot.
typedef struct {
    uint32_t id;
    char name[RADIATOR_NAME_BYTES + 1];
    uint8_t controller[4], radiator[4];
    bool has_report;
    int64_t report_ms;
    rf_report report;
    bool command_queued;
    rf_control_status command;
    int command_error;
    uint64_t request_id;
} radiator_record;
typedef struct {
    uint32_t next_id;
    size_t count;
    radiator_record records[CLEVERBOX_RADIATOR_CAPACITY];
} radiator_registry;

bool radiator_paired(const radiator_record *record);
bool radiator_name_valid(const char *name);
// Registry v2: version (2), count (LE16), next ID (LE32), then per record:
// ID (LE32), name (33, NUL-terminated), controller (4), radiator (4).
esp_err_t registry_load(radiator_registry *registry);
esp_err_t registry_save(const radiator_registry *registry);
#ifdef __cplusplus
}
#endif
