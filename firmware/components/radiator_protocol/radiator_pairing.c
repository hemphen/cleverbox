// SPDX-License-Identifier: MIT
#include "radiator_pairing.h"
#include <string.h>

bool rf_pair_active(const rf_pairing *p)
{
    return p->phase >= RF_PAIR_OFFER && p->phase <= RF_PAIR_WAIT_ACK;
}

bool rf_pair_start(rf_pairing *p, const uint8_t controller[4], const uint8_t candidate[4])
{
    if (rf_pair_active(p) || p->phase == RF_PAIR_COMPLETE ||
        (candidate[0] != 0xf1 && candidate[0] != 0xd0) ||
        controller[0] != candidate[0] || !memcmp(controller, candidate, 4) ||
        !(controller[1] | controller[2] | controller[3])) return false;
    *p = (rf_pairing){.phase = RF_PAIR_OFFER};
    memcpy(p->controller, controller, 4);
    memcpy(p->candidate, candidate, 4);
    return true;
}

void rf_pair_cancel(rf_pairing *p)
{
    if (rf_pair_active(p)) p->phase = RF_PAIR_CANCELLED;
}

void rf_pair_tick(rf_pairing *p, int64_t now_ms)
{
    if ((p->phase == RF_PAIR_WAIT_REPLY || p->phase == RF_PAIR_WAIT_ACK) &&
        now_ms >= p->deadline_ms) p->phase = RF_PAIR_TIMEOUT;
}

bool rf_pair_next(const rf_pairing *p, int64_t now_ms, rf_frame *out)
{
    if (p->phase == RF_PAIR_OFFER)
        return rf_short(out, p->controller, p->candidate, RF_OFFER);
    if (p->phase == RF_PAIR_BIND && now_ms >= p->send_at_ms)
        return rf_short(out, p->controller, p->radiator, RF_BIND);
    return false;
}

void rf_pair_sent(rf_pairing *p, int64_t now_ms, bool success)
{
    if (p->phase != RF_PAIR_OFFER && p->phase != RF_PAIR_BIND) return;
    if (!success) { p->phase = RF_PAIR_ERROR; return; }
    p->phase = p->phase == RF_PAIR_OFFER ? RF_PAIR_WAIT_REPLY : RF_PAIR_WAIT_ACK;
    // Prototype policy: one transmission per step, five seconds for its reply.
    p->deadline_ms = now_ms + 5000;
}

void rf_pair_receive(rf_pairing *p, int64_t now_ms, const rf_frame *frame)
{
    rf_pair_tick(p, now_ms);
    if (!rf_valid(frame) || memcmp(frame->bytes + 6, p->controller, 4)) return;
    if (p->phase == RF_PAIR_WAIT_REPLY && frame->size == 16 &&
        frame->bytes[10] == 0 && frame->bytes[11] == 0 &&
        frame->bytes[1] == p->candidate[0] && memcmp(frame->bytes + 1, p->controller, 4)) {
        memcpy(p->radiator, frame->bytes + 1, 4);
        p->phase = RF_PAIR_BIND;
        // A reversible timing assumption based on the observed one-second gaps.
        p->send_at_ms = now_ms + 1000;
    } else if (p->phase == RF_PAIR_WAIT_ACK && frame->size == 18 &&
               !memcmp(frame->bytes + 1, p->radiator, 4) &&
               !memcmp(frame->bytes + 10, "\x01\x00\x0a\x47", 4)) {
        p->phase = RF_PAIR_COMPLETE;
    }
}

const char *rf_pair_phase_name(rf_pair_phase phase)
{
    static const char *names[] = {"idle", "offer", "waiting_reply", "bind", "waiting_ack",
                                  "complete", "timeout", "cancelled", "error"};
    return phase >= RF_PAIR_IDLE && phase <= RF_PAIR_ERROR ? names[phase] : "error";
}
