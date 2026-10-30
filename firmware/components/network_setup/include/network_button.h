// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stdint.h>
#define NETWORK_BUTTON_DEBOUNCE_MS 40
#define NETWORK_BUTTON_HOLD_MS 5000
// Sampled by the network owner. No GPIO, RTOS, storage or RF dependencies.
typedef struct {
    bool initialized, raw_low, stable_low, armed, pressed;
    int64_t changed_ms, pressed_ms;
} network_button;
// Returns once on stable release after a deliberate runtime hold. Startup low
// cannot arm it; first observe a stable release. Time must be monotonic.
bool network_button_sample(network_button *, bool low, int64_t now);
