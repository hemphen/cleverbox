// SPDX-License-Identifier: MIT
#include "radiator_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static rf_frame parse(const char *hex)
{
    rf_frame f = {0};
    assert(rf_parse_hex(hex, f.bytes, sizeof(f.bytes), &f.size));
    assert(rf_valid(&f));
    return f;
}

static void matches(const rf_frame *f, const char *hex)
{
    char text[RF_MAX_FRAME * 2 + 1];
    assert(rf_valid(f)); rf_to_hex(f, text); assert(strcmp(text, hex) == 0);
}

int main(void)
{
    const uint8_t controller[] = {0xf1,1,2,3}, radiator[] = {0xf1,4,5,6};
    const uint8_t candidate[] = {0xf1,0x10,0,1};
    rf_frame f;
    assert(rf_short(&f, controller, candidate, RF_OFFER));
    matches(&f, "0FF1010203C6F1100001000003011FF3EA1F");
    assert(rf_short(&f, controller, radiator, RF_BIND));
    matches(&f, "0DF1010203C6F10405060100AA1B1F72");
    assert(rf_short(&f, controller, radiator, RF_POLL));
    matches(&f, "0DF1010203C6F10405061900A01B4377");
    assert(!rf_short(&f, controller, radiator, (rf_command)99));
    const char *state_hex = "38F1010203C6F10405064500004B02A83B004C00008E000084CC00000000972030049402BA00E100641919DA000000005B0078B20C0000A20F60D9";
    rf_frame state = parse(state_hex);
    assert(rf_set_target(&f, &state, RF_COMFORT, 2100)); matches(&f, state_hex);
    for (int mode = 0; mode <= 3; ++mode) {
        assert(rf_set_target(&f, &state, (rf_mode)mode, 2050));
        assert(rf_valid(&f));
        unsigned raw = mode == RF_OFF ? 320 : 689;
        assert(f.bytes[35] == raw >> 8 && f.bytes[36] == (raw & 255));
        assert(f.bytes[37] == mode);
        for (size_t i = 0; i < 55; ++i)
            if (i < 35 || i > 37) assert(f.bytes[i] == state.bytes[i]);
    }
    assert(!rf_set_target(&f, &state, RF_COMFORT, 499));
    assert(!rf_set_target(&f, &state, RF_COMFORT, 3001));
    assert(!rf_set_target(&f, &state, (rf_mode)8, 2000));
    rf_frame report = parse("23F1040506C6F1010203DF203000404B02A88A02BA00E100641919DA000A000A0A47CD48C59D");
    rf_report decoded;
    assert(rf_decode_report(&report, &decoded));
    assert(memcmp(decoded.source, radiator, 4) == 0);
    assert(memcmp(decoded.destination, controller, 4) == 0);
    assert(decoded.temperature_available && decoded.temperature_centi_c == 2000);
    assert(decoded.target_available && decoded.target_centi_c == 2100);
    assert(decoded.mode == 0);
    assert(!rf_decode_report(&state, &decoded));
    assert(!rf_decode_report(&report, NULL));
    for (size_t i = 0; i < report.size; ++i) {
        rf_frame bad = report; bad.bytes[i] ^= 1;
        assert(!rf_decode_report(&bad, &decoded));
    }
    for (size_t n = 0; n < report.size; ++n) {
        rf_frame bad = report; bad.size = n;
        assert(!rf_decode_report(&bad, &decoded));
    }
    assert(!rf_set_target(&f, &report, RF_COMFORT, 2000));
    for (size_t i = 0; i < state.size; ++i) {
        rf_frame bad = state; bad.bytes[i] ^= 1;
        assert(!rf_valid(&bad));
    }
    for (size_t n = 0; n < state.size; ++n) {
        rf_frame bad = state; bad.size = n; assert(!rf_valid(&bad));
    }
    // Unavailable temperatures remain unavailable; unknown mode stays raw.
    rf_frame unavailable = parse("23F1040506C6F1010203DF203000404B084C8A084C63E100641919DA000A000A0A471D37039E");
    assert(rf_decode_report(&unavailable, &decoded));
    assert(!decoded.temperature_available && !decoded.target_available && decoded.mode == 99);
    rf_frame cold = parse("23F1040506C6F1010203DF203000404B00E68A014001E100641919DA000A000A0A47823DC7B2");
    assert(rf_decode_report(&cold, &decoded));
    assert(decoded.temperature_centi_c == -500 && decoded.target_centi_c == 0 && decoded.mode == 1);
    // An unfamiliar, CRC-valid layout is retained as a frame, not decoded as a report.
    rf_frame unknown = parse("23F1040506C6F1010203DF203000404C02A88A02BA00E100641919DA000A000A0A47783C95DA");
    assert(!rf_decode_report(&unknown, &decoded));
    size_t n; uint8_t small[4];
    assert(!rf_parse_hex("abc", small, sizeof(small), &n));
    assert(!rf_parse_hex("gg", small, sizeof(small), &n));
    assert(!rf_parse_hex("0011223344", small, sizeof(small), &n));
    puts("Protocol vectors, report decoding, unavailable values and invalid frames: passed");
}
