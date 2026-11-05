// SPDX-License-Identifier: MIT
#pragma once
#include "cJSON.h"
#ifdef __cplusplus
extern "C" {
#endif
void simulation_init(void);
void simulation_status(cJSON *);
void simulation_advance(int milliseconds);
int simulation_device(int index);
int simulation_scenario(const char *);
int simulation_inject(const char *hex);
int simulation_radio_fault(const char *stage, int code);
void host_network_tick(void);
#ifdef __cplusplus
}
#endif
