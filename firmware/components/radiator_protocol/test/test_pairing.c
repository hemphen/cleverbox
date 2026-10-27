// SPDX-License-Identifier: MIT
#include "radiator_pairing.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const uint8_t controller[4] = {0xf1, 1, 2, 3}, candidate[4] = {0xf1, 0x10, 0, 1};
static rf_frame parse(const char *hex)
{
    rf_frame f = {0};
    assert(rf_parse_hex(hex, f.bytes, sizeof(f.bytes), &f.size));
    assert(rf_valid(&f));
    return f;
}
static void start(rf_pairing *p)
{
    *p = (rf_pairing){0};
    assert(rf_pair_start(p, controller, candidate));
    rf_pair_sent(p, 100, true);
}
int main(void)
{
    rf_pairing p = {0}; rf_frame out;
    assert(!rf_pair_next(&p, 0, &out)); // No inclusion at boot.
    assert(rf_pair_start(&p, controller, candidate));
    assert(!rf_pair_start(&p, controller, candidate));
    assert(rf_pair_next(&p, 0, &out));
    char hex[RF_MAX_FRAME * 2 + 1]; rf_to_hex(&out, hex);
    assert(!strcmp(hex, "0FF1010203C6F1100001000003011FF3EA1F"));
    rf_pair_sent(&p, 100, true);
    assert(!rf_pair_next(&p, 101, &out)); // No automatic retransmit.
    rf_frame reply = parse("0DF1040506C6F1010203000080E1D2A1");
    rf_frame ack = parse("0FF1040506C6F101020301000A47E6E64D5A");
    rf_pair_receive(&p, 200, &ack); assert(p.phase == RF_PAIR_WAIT_REPLY);
    rf_pair_receive(&p, 300, &reply); assert(p.phase == RF_PAIR_BIND);
    assert(!rf_pair_next(&p, 1299, &out));
    assert(rf_pair_next(&p, 1300, &out));
    rf_to_hex(&out, hex);
    assert(!strcmp(hex, "0DF1010203C6F10405060100AA1B1F72"));
    rf_pair_sent(&p, 1300, true);
    rf_pair_receive(&p, 1400, &reply); assert(p.phase == RF_PAIR_WAIT_ACK);
    rf_pair_receive(&p, 1500, &ack); assert(p.phase == RF_PAIR_COMPLETE);
    assert(!rf_pair_active(&p) && !rf_pair_next(&p, 99999, &out));
    assert(!rf_pair_start(&p, controller, candidate)); // Save/recover before restarting.
    rf_pair_receive(&p, 1600, &ack); assert(p.phase == RF_PAIR_COMPLETE);

    for (size_t i = 0; i < reply.size; ++i) {
        start(&p); rf_frame bad = reply; bad.bytes[i] ^= 1;
        rf_pair_receive(&p, 200, &bad); assert(p.phase == RF_PAIR_WAIT_REPLY);
    }
    start(&p); rf_pair_tick(&p, 5100); assert(p.phase == RF_PAIR_TIMEOUT);
    rf_pair_receive(&p, 5101, &reply); assert(p.phase == RF_PAIR_TIMEOUT);
    assert(rf_pair_start(&p, controller, candidate));
    rf_pair_sent(&p, 6000, false); assert(p.phase == RF_PAIR_ERROR);
    assert(!rf_pair_next(&p, 99999, &out));
    start(&p); rf_pair_cancel(&p); rf_pair_receive(&p, 200, &reply);
    assert(p.phase == RF_PAIR_CANCELLED && !rf_pair_next(&p, 300, &out));
    start(&p); rf_pair_receive(&p, 200, &reply); rf_pair_cancel(&p);
    assert(!rf_pair_next(&p, 1200, &out)); // Cancel during bind delay.
    start(&p); rf_pair_receive(&p, 200, &reply); rf_pair_sent(&p, 1200, true);
    rf_pair_cancel(&p); rf_pair_receive(&p, 1300, &ack); assert(p.phase == RF_PAIR_CANCELLED);
    start(&p); rf_pair_receive(&p, 200, &reply); rf_pair_sent(&p, 1200, true);
    rf_pair_receive(&p, 6200, &ack); assert(p.phase == RF_PAIR_TIMEOUT);

    // CRC-valid traffic with a different destination must not advance inclusion.
    rf_frame poll; assert(rf_short(&poll, controller, candidate, RF_POLL));
    start(&p); rf_pair_receive(&p, 200, &poll); assert(p.phase == RF_PAIR_WAIT_REPLY);
    p.controller[3] ^= 1;
    rf_pair_receive(&p, 300, &reply); assert(p.phase == RF_PAIR_WAIT_REPLY);
    start(&p); rf_pair_receive(&p, 200, &reply); rf_pair_sent(&p, 1200, true);
    p.radiator[3] ^= 1;
    rf_pair_receive(&p, 1300, &ack); assert(p.phase == RF_PAIR_WAIT_ACK);
    p = (rf_pairing){0}; uint8_t wrong_family[4] = {0xd0,1,2,3};
    assert(!rf_pair_start(&p, wrong_family, candidate));
    assert(!rf_pair_start(&p, candidate, candidate));
    puts("Pairing exchange, addressing, cancellation, timeout and TX failure: passed");
}
