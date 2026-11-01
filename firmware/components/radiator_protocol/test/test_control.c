// SPDX-License-Identifier: MIT
#include "radiator_control.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const uint8_t controller[4] = {0xf1,1,2,3}, radiator[4] = {0xf1,4,5,6};
static const char *state_hex = "38F1010203C6F10405064500004B02A83B004C00008E000084CC00000000972030049402BA00E100641919DA000000005B0078B20C0000A20F60D9";
static const char *report_hex = "23F1040506C6F1010203DF203000404B02A88A02BA00E100641919DA000A000A0A47CD48C59D";
static rf_frame parse(const char *hex)
{
    rf_frame f = {0};
    assert(rf_parse_hex(hex, f.bytes, sizeof(f.bytes), &f.size) && rf_valid(&f));
    return f;
}
static rf_control_request request(void)
{
    rf_control_request r = {.mode = RF_COMFORT,
                            .centi_c = 2050, .clock_seconds = 86399};
    return r;
}
static void start(rf_control *c)
{
    *c = (rf_control){0};
    rf_control_request r = request();
    assert(rf_control_start(c, &r, controller, radiator, 1000));
    rf_control_sent(c, 1000, true);
}

static void expect_frame(const rf_frame *actual, const char *expected_hex)
{
    rf_frame expected = parse(expected_hex);
    assert(actual->size == 59 && actual->bytes[0] == 0x38);
    assert(actual->size == expected.size && !memcmp(actual->bytes, expected.bytes, expected.size));
}

static void complete_state_vectors(void)
{
    rf_frame state = parse(state_hex), report = parse(report_hex), out;
    // The specification's complete synthetic state: all bytes, both CRCs.
    assert(rf_live_state(&out, &state, &report, controller, radiator, RF_COMFORT, 2100, 43200));
    expect_frame(&out, state_hex);
    // Independently assembled from the fixed record order in protocol b70fab79,
    // with CRCs calculated by clevertouch-radio 7706b90c radiatord/decoder.py
    // crc16 (not this codec). Raw words = 320 + 18*C: 410,689,860,320,446,599.
    // Clock 23:59:59 = 17 3B 3B; 5B and the opaque values are fixture data, NOT
    // adopted defaults. See docs/architecture.md (Templates) for the byte mapping.
    static const struct { rf_mode mode; int target; const char *hex; } vectors[] = {
        {RF_COMFORT, 500, "38F1010203C6F10405064500004B02A83B004C00008E000084CC000000009720300494019A00E100641919DA000000005B0078B2173B3B4F1FC430"},
        {RF_COMFORT, 2050, "38F1010203C6F10405064500004B02A83B004C00008E000084CC00000000972030049402B100E100641919DA000000005B0078B2173B3B26CE6271"},
        {RF_COMFORT, 3000, "38F1010203C6F10405064500004B02A83B004C00008E000084CC000000009720300494035C00E100641919DA000000005B0078B2173B3B826413A3"},
        {RF_OFF, 0, "38F1010203C6F10405064500004B02A83B004C00008E000084CC000000009720300494014001E100641919DA000000005B0078B2173B3B7DE8F50D"},
        {RF_FROST, 700, "38F1010203C6F10405064500004B02A83B004C00008E000084CC00000000972030049401BE02E100641919DA000000005B0078B2173B3B9450290A"},
        {RF_ECO, 1550, "38F1010203C6F10405064500004B02A83B004C00008E000084CC000000009720300494025703E100641919DA000000005B0078B2173B3BE9D4B021"},
    };
    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
        assert(rf_live_state(&out, &state, &report, controller, radiator,
                             vectors[i].mode, vectors[i].target, 86399));
        expect_frame(&out, vectors[i].hex);
    }
    // 97 echoes the answered report's DF (byte 0, and byte 2 bit 0 = heating),
    // keeping the template's other bits and its 5B. Templates and reports are the
    // vectors controller_state, state_boost, radiator_report and report_heating
    // from protocol 5dc2bcf; the report with DF byte 0 = 00 and all expected
    // frames were built by radiator-rf 6977a94
    // controller_state(..., mode=0, setpoint_C=21.0, clock=(12,0,0)).
    static const char *state_boost_hex = "38F1010203C6F10405064500004B02A83B004C00008E000084CC000000009720300594029604E100641919DA000000005B0075B20C000086A53F89";
    static const char *report_heating_hex = "23F1040506C6F1010203DF203001404B02A88A02BA00E100641919DA000A000A0A47DD99311E";
    static const char *report_df0_hex = "23F1040506C6F1010203DF003000404B02A88A02BA00E100641919DA000A000A0A475751B366";
    const struct { const char *state, *report, *hex; } echoes[] = {
        // Heating report: 97 byte 2 bit 0 set (203004 -> 203005).
        {state_hex, report_heating_hex, "38F1010203C6F10405064500004B02A83B004C00008E000084CC00000000972030059402BA00E100641919DA000000005B0078B20C000072C3640A"},
        // Template that echoed a heating report, answering one that is not: the bit
        // clears (203005 -> 203004) and the template's Boost time 117 (0075) stays.
        {state_boost_hex, report_hex, "38F1010203C6F10405064500004B02A83B004C00008E000084CC00000000972030049402BA00E100641919DA000000005B0075B20C00008FCE818F"},
        // DF byte 0 differs from the template's 97 byte 0: the report wins.
        {state_hex, report_df0_hex, "38F1010203C6F10405064500004B02A83B004C00008E000084CC00000000970030049402BA00E100641919DA000000005B0078B20C00003B641056"},
    };
    for (size_t i = 0; i < sizeof(echoes) / sizeof(echoes[0]); ++i) {
        rf_frame template_frame = parse(echoes[i].state), answered = parse(echoes[i].report);
        assert(rf_live_state(&out, &template_frame, &answered, controller, radiator,
                             RF_COMFORT, 2100, 43200));
        expect_frame(&out, echoes[i].hex);
    }
    rf_frame empty = {0};
    assert(!rf_live_state(&out, &empty, &report, controller, radiator, RF_COMFORT, 2000, 0));
    assert(!rf_live_state(&out, &state, &empty, controller, radiator, RF_COMFORT, 2000, 0));
    assert(!rf_live_state(&out, &state, &report, controller, radiator, RF_COMFORT, 3001, 0));
}
static void generated_context_contract(void)
{
    rf_control c; rf_frame out;
    rf_frame report = parse(report_hex);
    rf_frame newer = parse("23F1040506C6F1010203DF203000404B084C8A02BA00E100373764DA000A000A0A4705F7FAB5");
    rf_frame incompatible = parse("23F1040506C6F1010203DF203000404B02A88A02BA00E100641919DA000A000A0A460C88C106");
    start(&c);
    rf_control_receive(&c, 999, &report); // Impossible earlier-boot context.
    rf_control_receive(&c, 1100, &incompatible);
    assert(c.status.phase == RF_CONTROL_WAIT_REPORT && !c.report_size);
    rf_control_receive(&c, 1200, &report);
    assert(c.report_received_ms == 1200 && c.deadline_ms == 151000);
    assert(rf_control_next(&c, 2000, &out));
    expect_frame(&out, "38F1010203C6F10405064500004B02A83B004C00008E000084CC00000000972030049402B100E100641919DA000000005B0078B2000000C429F592");
    assert(c.report_received_ms == 1200); // next() never refreshes age.
    rf_control_receive(&c, 2100, &newer); // Ready attempt retains its answering report.
    assert(c.report_received_ms == 1200);
    assert(rf_control_next(&c, 2200, &out) && out.bytes[14] == 2);
    rf_control snapshot = c;
    // These injected corrupt machine timestamps cannot pass the builder gate,
    // even if a future refactor were to widen the ready deadline.
    c.report_received_ms = -1; assert(!rf_control_next(&c, 2200, &out));
    c.report_received_ms = 2201; assert(!rf_control_next(&c, 2200, &out));
    c.report_received_ms = 1200; c.deadline_ms = 400000;
    assert(!rf_control_next(&c, 301200, &out));
    c = snapshot;
    rf_control_sent(&c, 2200, true);
    rf_control_receive(&c, 3200, &newer);
    assert(c.report_received_ms == 3200 && c.status.phase == RF_CONTROL_SEND);
    assert(rf_control_next(&c, 4000, &out));
    assert(out.bytes[14] == 8 && out.bytes[15] == 0x4c);
    assert(!memcmp(out.bytes + 39, newer.bytes + 23, 4));
    assert(out.bytes[49] == 0 && out.bytes[50] == 120 && out.bytes[54] == 2);
    rf_control_sent(&c, 4000, true);
    rf_control_receive(&c, 4100, &report);
    assert(c.status.attempts == 2 && c.status.phase == RF_CONTROL_WAIT_READBACK);
    assert(!rf_control_next(&c, 4200, &out));
}

int main(void)
{
    complete_state_vectors();
    generated_context_contract();
    rf_frame state = parse(state_hex), report = parse(report_hex), out;
    rf_control_request r = request();
    rf_control c = {0};
    assert(!rf_control_next(&c, 0, &out));
    rf_control_receive(&c, 1, &report);
    assert(c.status.phase == RF_CONTROL_IDLE); // Reboot never replays a command.
    assert(rf_control_start(&c, &r, controller, radiator, 1000));
    assert(!rf_control_start(&c, &r, controller, radiator, 1000));
    assert(rf_control_next(&c, 1000, &out));
    char hex[RF_MAX_FRAME * 2 + 1]; rf_to_hex(&out, hex);
    assert(!strcmp(hex, "0DF1010203C6F10405061900A01B4377"));
    rf_control_sent(&c, 1000, true);
    assert(c.status.phase == RF_CONTROL_WAIT_REPORT && c.status.attempts == 0);
    assert(!rf_control_next(&c, 1500, &out));
    rf_frame ack = parse("0FF1040506C6F101020301000A47E6E64D5A");
    rf_control_receive(&c, 1500, &ack);
    assert(c.status.phase == RF_CONTROL_WAIT_REPORT);
    rf_control_receive(&c, 2000, &report);
    assert(c.status.phase == RF_CONTROL_SEND);
    assert(rf_control_next(&c, 4000, &out) && rf_valid(&out));
    assert(out.bytes[35] == 2 && out.bytes[36] == 0xb1 && out.bytes[37] == 0);
    assert(out.bytes[52] == 0 && out.bytes[53] == 0 && out.bytes[54] == 2); // midnight rollover
    assert(!memcmp(out.bytes + 14, report.bytes + 16, 2));
    assert(!memcmp(out.bytes + 39, report.bytes + 23, 4));
    for (int i = 44; i < 48; ++i) assert(out.bytes[i] == 0);
    for (int i = 10; i < 55; ++i)
        if (!(i >= 14 && i <= 15) && !(i >= 35 && i <= 37) &&
            !(i >= 39 && i <= 42) && !(i >= 44 && i <= 47) && !(i >= 52 && i <= 54))
            assert(out.bytes[i] == state.bytes[i]); // fixed generated profile fields
    rf_control_sent(&c, 4000, true);
    assert(c.status.phase == RF_CONTROL_WAIT_READBACK && c.status.attempts == 1);
    rf_control_receive(&c, 4500, &ack);
    assert(c.status.phase == RF_CONTROL_WAIT_READBACK); // ACK is not application proof.
    rf_control_receive(&c, 5000, &report);
    assert(c.status.phase == RF_CONTROL_SEND);
    assert(rf_control_next(&c, 5000, &out));
    rf_control_sent(&c, 5000, false);
    assert(c.status.phase == RF_CONTROL_ERROR && c.status.attempts == 2);
    assert(!rf_control_next(&c, 5001, &out));
    rf_frame match = parse("23F1040506C6F1010203DF203000404B02A88A02B100E100641919DA000A000A0A47C783C11D");
    rf_control_receive(&c, 6000, &match);
    assert(c.status.phase == RF_CONTROL_MATCHED && c.status.matched_ms == 6000);
    assert(!rf_control_next(&c, 6001, &out));
    start(&c); rf_control_receive(&c, 1100, &report); rf_control_sent(&c, 1200, true);
    rf_control_receive(&c, 1300, &report); rf_control_sent(&c, 1400, true);
    rf_control_receive(&c, 1500, &report);
    assert(c.status.phase == RF_CONTROL_WAIT_READBACK && c.status.attempts == 2);
    assert(!rf_control_next(&c, 1600, &out)); // At most two state attempts.
    rf_control_tick(&c, 151400); assert(c.status.phase == RF_CONTROL_TIMEOUT);
    rf_control_receive(&c, 151500, &report);
    assert(!rf_control_next(&c, 151500, &out)); // A late mismatch does not restart TX.
    rf_control_receive(&c, 151600, &match); assert(c.status.phase == RF_CONTROL_MATCHED);
    start(&c); rf_control_tick(&c, 151000); assert(c.status.phase == RF_CONTROL_TIMEOUT);
    rf_control_receive(&c, 151001, &match);
    assert(c.status.phase == RF_CONTROL_TIMEOUT && !rf_control_next(&c, 151002, &out));
    start(&c); rf_control_sent(&c, 1001, false); // Sent callback ignored outside a TX phase.
    assert(c.status.phase == RF_CONTROL_WAIT_REPORT);
    c = (rf_control){0}; assert(rf_control_start(&c, &r, controller, radiator, 1000));
    rf_control_sent(&c, 1001, false); assert(c.status.phase == RF_CONTROL_ERROR);
    rf_control_receive(&c, 2000, &match); assert(c.status.phase == RF_CONTROL_ERROR);
    rf_frame wrong_dest = parse("23F1040506C6F1010202DF203000404B02A88A02BA00E100641919DA000A000A0A4799B10073");
    rf_frame wrong_source = parse("23F1040507C6F1010203DF203000404B02A88A02BA00E100641919DA000A000A0A47CC18D07D");
    start(&c); rf_control_receive(&c, 1100, &wrong_dest);
    rf_control_receive(&c, 1200, &wrong_source);
    assert(c.status.phase == RF_CONTROL_WAIT_REPORT);
    rf_frame bad = report; bad.bytes[16] ^= 1;
    rf_control_receive(&c, 1300, &bad); assert(c.status.phase == RF_CONTROL_WAIT_REPORT);
    start(&c); rf_control_receive(&c, 1100, &report); rf_control_sent(&c, 1200, true);
    rf_control_receive(&c, 1300, &wrong_dest); rf_control_receive(&c, 1400, &wrong_source);
    assert(c.status.phase == RF_CONTROL_WAIT_READBACK);
    rf_frame fresh = parse("23F1040506C6F1010203DF203000404B084C8A02BA00E100373764DA000A000A0A4705F7FAB5");
    rf_frame unavailable = parse("23F1040506C6F1010203DF203000404B084C8A084C00E100373764DA000A000A0A477906FA25");
    rf_frame updated;
    assert(rf_live_state(&updated, &state, &fresh, controller, radiator, RF_OFF, 0, 12345));
    assert(updated.bytes[14] == 8 && updated.bytes[15] == 0x4c); // unavailable echo
    assert(updated.bytes[35] == 1 && updated.bytes[36] == 0x40 && updated.bytes[37] == 1);
    assert(!memcmp(updated.bytes + 39, fresh.bytes + 23, 4));
    assert(updated.bytes[52] == 3 && updated.bytes[53] == 25 && updated.bytes[54] == 45);
    expect_frame(&updated, "38F1010203C6F10405064500004B084C3B004C00008E000084CC000000009720300494014001E100373764DA000000005B0078B203192D3A8CFC68");
    assert(!rf_live_state(&updated, &state, &fresh, controller, radiator, RF_ECO, 2000, 86400));
    assert(!rf_live_state(&updated, &state, &wrong_dest, controller, radiator, RF_ECO, 2000, 0));
    start(&c); rf_control_receive(&c, 1100, &report); rf_control_sent(&c, 1200, true);
    rf_control_receive(&c, 1300, &unavailable); assert(c.status.phase != RF_CONTROL_MATCHED);
    c = (rf_control){0}; r.clock_seconds = 86400;
    assert(!rf_control_start(&c, &r, controller, radiator, 0));
    r = request(); r.mode = (rf_mode)8; assert(!rf_control_start(&c, &r, controller, radiator, 0));
    r = request(); r.centi_c = 499; assert(!rf_control_start(&c, &r, controller, radiator, 0));
    uint8_t other[4] = {0xf1,4,5,7}; r = request();
    assert(!rf_control_start(&c, &r, controller, controller, 0));
    assert(rf_control_start(&c, &r, controller, other, 0));
    c = (rf_control){0};
    r.centi_c = 2051; assert(rf_control_start(&c, &r, controller, radiator, 1000));
    rf_control_sent(&c, 1000, true); rf_control_receive(&c, 1100, &report);
    rf_control_sent(&c, 1200, true); rf_control_receive(&c, 1300, &match);
    assert(c.status.phase == RF_CONTROL_MATCHED); // Compare encoded target, not unrepresentable hundredths.
    rf_frame readdressed_report = parse("23F1040506C6F1070809DF203000404B02A88A02BA00E100641919DA000A000A0A47F4BE58B2");
    const uint8_t new_controller[4] = {0xf1,7,8,9}; r = request();
    assert(rf_live_state(&updated, &state, &readdressed_report, new_controller, radiator, RF_COMFORT, 2050, 0));
    assert(!memcmp(updated.bytes + 1, new_controller, 4) && !memcmp(updated.bytes + 6, radiator, 4));
    expect_frame(&updated, "38F1070809C6F10405064500004B02A83B004C00008E000084CC00000000972030049402B100E100641919DA000000005B0078B2000000B1BED423");
    static const char *mode_reports[] = {
        "23F1040506C6F1010203DF203000404B02A88A02B100E100641919DA000A000A0A47C783C11D",
        "23F1040506C6F1010203DF203000404B02A88A014001E100641919DA000A000A0A4776324BA1",
        "23F1040506C6F1010203DF203000404B02A88A02B102E100641919DA000A000A0A47C0C1509E",
        "23F1040506C6F1010203DF203000404B02A88A02B103E100641919DA000A000A0A47C240DE9D",
    };
    for (int mode = 0; mode < 4; ++mode) {
        c = (rf_control){0}; r = request(); r.mode = (rf_mode)mode;
        assert(rf_control_start(&c, &r, controller, radiator, 1000));
        rf_control_sent(&c, 1000, true); rf_control_receive(&c, 1100, &report);
        assert(rf_control_next(&c, 1200, &out));
        rf_control_sent(&c, 1200, true);
        rf_frame mode_report = parse(mode_reports[mode]);
        if (mode != 0) {
            rf_control_receive(&c, 1300, &match);
            assert(c.status.phase != RF_CONTROL_MATCHED); // Target alone is insufficient.
        }
        rf_control_receive(&c, 1400, &mode_report);
        assert(c.status.phase == RF_CONTROL_MATCHED);
    }
    c = (rf_control){0}; r = request();
    assert(rf_control_start(&c, &r, controller, radiator, 1000));
    rf_control unchanged = c;
    assert(rf_control_next(&c, 150999, &out) && !memcmp(&c, &unchanged, sizeof(c)));
    assert(!rf_control_next(&c, 151000, &out));
    rf_control_tick(&c, 151000); assert(c.status.phase == RF_CONTROL_TIMEOUT && !c.status.attempts);
    start(&c); rf_control_receive(&c, 150999, &report);
    assert(c.status.phase == RF_CONTROL_SEND && c.deadline_ms == 151000);
    assert(!rf_control_next(&c, 151000, &out));
    rf_control_tick(&c, 151000); assert(c.status.phase == RF_CONTROL_TIMEOUT && !c.status.attempts);
    puts("Live state refresh, delivery/readback, retry bounds, timeouts and late reports: passed");
}
