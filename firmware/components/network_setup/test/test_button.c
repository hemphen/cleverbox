// SPDX-License-Identifier: MIT
#include "network_button.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    network_button b = {0};
    // Startup held/stuck low: arbitrarily long holds cannot arm or repeat.
    assert(!network_button_sample(&b, true, 0));
    assert(!network_button_sample(&b, true, 100000));
    assert(!network_button_sample(&b, false, 100001));
    assert(!network_button_sample(&b, false, 100040)); assert(!b.armed);
    assert(!network_button_sample(&b, true, 100041)); // release bounce
    assert(!network_button_sample(&b, false, 100042));
    assert(!network_button_sample(&b, false, 100082)); assert(b.armed);
    // Press bounce under debounce is ignored.
    assert(!network_button_sample(&b, true, 100100));
    assert(!network_button_sample(&b, false, 100139));
    assert(!network_button_sample(&b, false, 100179)); assert(!b.pressed);
    // Short and threshold boundary holds. Release debounce never rounds up.
    int64_t t = 200000;
    const int durations[] = {1, 39, 40, 1000, 4999, 5000, 5001, 100000};
    for (unsigned i = 0; i < sizeof(durations) / sizeof(*durations); ++i) {
        int hold = durations[i];
        assert(!network_button_sample(&b, true, t));
        if (hold >= 40) assert(!network_button_sample(&b, true, t + 40));
        assert(!network_button_sample(&b, true, t + hold));
        assert(!network_button_sample(&b, false, t + hold));
        assert(!network_button_sample(&b, false, t + hold + 39));
        assert(network_button_sample(&b, false, t + hold + 40) == (hold >= 5000));
        assert(!network_button_sample(&b, false, t + hold + 100));
        t += hold + 200;
    }
    // Release bounce produces one action only after stable release.
    assert(!network_button_sample(&b, true, t));
    assert(!network_button_sample(&b, true, t + 40));
    assert(!network_button_sample(&b, false, t + 5000));
    assert(!network_button_sample(&b, true, t + 5020));
    assert(!network_button_sample(&b, false, t + 5030));
    assert(network_button_sample(&b, false, t + 5070));
    assert(!network_button_sample(&b, false, t + 10000));
    // Startup already released still requires the full stable interval.
    b = (network_button){0};
    assert(!network_button_sample(&b, false, 0));
    assert(!network_button_sample(&b, false, 39)); assert(!b.armed);
    assert(!network_button_sample(&b, false, 40)); assert(b.armed);
    puts("Button startup arming, debounce, exact hold boundaries and one-shot release: passed");
}
