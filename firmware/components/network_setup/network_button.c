// SPDX-License-Identifier: MIT
#include "network_button.h"
bool network_button_sample(network_button *b, bool low, int64_t now) {
    if (!b->initialized) {
        b->initialized = true; b->raw_low = b->stable_low = low;
        b->changed_ms = now;
        return false;
    }
    if (low != b->raw_low) { b->raw_low = low; b->changed_ms = now; }
    if (now - b->changed_ms < NETWORK_BUTTON_DEBOUNCE_MS) return false;
    if (!b->armed) {
        if (!low) { b->armed = true; b->stable_low = false; }
        return false;
    }
    if (low == b->stable_low) return false;
    b->stable_low = low;
    if (low) {
        b->pressed = true; b->pressed_ms = b->changed_ms;
        return false;
    }
    // Exclude release debounce time: a 4999 ms hold must not become a long
    // press merely because accepting its release takes another 40 ms.
    bool fire = b->pressed && b->changed_ms - b->pressed_ms >= NETWORK_BUTTON_HOLD_MS;
    b->pressed = false;
    return fire;
}
