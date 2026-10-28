// SPDX-License-Identifier: MIT
#pragma once
#include "cJSON.h"
#ifdef __cplusplus
extern "C" {
#endif
void simulation_status(cJSON *);
void simulation_init(void);
void simulation_advance(int milliseconds);
int simulation_scenario(const char *);
int simulation_inject(const char *);
void simulation_fixture(void);
#ifdef __cplusplus
}
#endif
