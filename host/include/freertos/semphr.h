// SPDX-License-Identifier: MIT
#pragma once
#include <pthread.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
typedef pthread_mutex_t *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    pthread_mutex_t *m = malloc(sizeof(*m));
    if (!m) return NULL;
    if (pthread_mutex_init(m, NULL)) { free(m); return NULL; }
    return m;
}
static inline int xSemaphoreTake(SemaphoreHandle_t m, int wait) { (void)wait; return pthread_mutex_lock(m) == 0; }
static inline int xSemaphoreGive(SemaphoreHandle_t m) { return pthread_mutex_unlock(m) == 0; }
