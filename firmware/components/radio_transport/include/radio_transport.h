// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    bool ready;
    int error;
    int chip_version;
} radio_status;
// Starts receive-only operation. Failure leaves the HTTP interface available.
void radio_transport_start(void);
radio_status radio_transport_status(void);
#ifdef __cplusplus
}
#endif
