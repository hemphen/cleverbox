// SPDX-License-Identifier: MIT
#include "radiator_control.h"
#include <string.h>

bool rf_control_active(const rf_control_status *s)
{
    return s->phase >= RF_CONTROL_POLL && s->phase <= RF_CONTROL_WAIT_READBACK;
}

bool rf_control_start(rf_control *c, const rf_control_request *request,
                      const uint8_t controller[4], const uint8_t radiator[4], int64_t now_ms)
{
    if (!c || !request || !controller || !radiator || now_ms < 0 ||
        rf_control_active(&c->status) || request->clock_seconds >= 86400 ||
        request->mode < RF_COMFORT || request->mode > RF_ECO ||
        (request->mode != RF_OFF && (request->centi_c < 500 || request->centi_c > 3000)) ||
        (controller[0] != 0xf1 && controller[0] != 0xd0) || controller[0] != radiator[0] ||
        !(controller[1] | controller[2] | controller[3]) || !memcmp(controller, radiator, 4)) return false;
    *c = (rf_control){0};
    c->status.phase = RF_CONTROL_POLL;
    c->status.mode = request->mode;
    c->status.centi_c = request->mode == RF_OFF ? 0 : request->centi_c;
    c->mode = request->mode; c->centi_c = c->status.centi_c;
    c->clock_seconds = request->clock_seconds;
    c->started_ms = now_ms;
    // Bound the initial poll from admission; ready SEND keeps its report deadline.
    c->deadline_ms = now_ms + RF_CONTROL_WAIT_MS;
    memcpy(c->controller, controller, 4);
    memcpy(c->radiator, radiator, 4);
    return true;
}

void rf_control_tick(rf_control *c, int64_t now_ms)
{
    if (rf_control_active(&c->status) && now_ms >= c->deadline_ms) c->status.phase = RF_CONTROL_TIMEOUT;
}

bool rf_control_next(const rf_control *c, int64_t now_ms, rf_frame *out)
{
    if (now_ms < c->started_ms || now_ms >= c->deadline_ms) return false;
    if (c->status.phase == RF_CONTROL_POLL)
        return rf_short(out, c->controller, c->radiator, RF_POLL);
    if (c->status.phase == RF_CONTROL_SEND) {
        rf_frame report = {0};
        if (c->report_size > sizeof(c->report_bytes)) return false;
        report.size = c->report_size; memcpy(report.bytes, c->report_bytes, report.size);
        rf_touch_e3_request request = {0};
        memcpy(request.controller, c->controller, 4); memcpy(request.radiator, c->radiator, 4);
        request.mode = c->mode; request.centi_c = c->centi_c;
        request.clock_seconds = (unsigned)((c->clock_seconds +
            (uint64_t)((now_ms - c->started_ms) / 1000)) % 86400);
        // Observed non-Boost default/reset value, not a timer or synthetic-fixture
        // default. Evidence: clevertouch-radio b951a717, Boost time (5B).
        // See docs/controller-profile.md. No captured state influences output.
        request.boost_minutes = 120;
        rf_report_context context = {.frame = &report, .received_ms = c->report_received_ms};
        return rf_touch_e3_state(out, &request, &context, now_ms);
    }
    return false;
}

void rf_control_sent(rf_control *c, int64_t now_ms, bool success)
{
    rf_control_phase phase = c->status.phase;
    if (phase != RF_CONTROL_POLL && phase != RF_CONTROL_SEND) return;
    if (phase == RF_CONTROL_SEND) ++c->status.attempts; // TX errors can still mean bytes went out.
    c->status.phase = !success ? RF_CONTROL_ERROR :
                      phase == RF_CONTROL_POLL ? RF_CONTROL_WAIT_REPORT : RF_CONTROL_WAIT_READBACK;
    c->deadline_ms = now_ms + RF_CONTROL_WAIT_MS;
}

bool rf_control_reconcile(rf_control_status *s, int64_t now_ms, const rf_frame *frame)
{
    rf_report report;
    if (!s->attempts || s->phase == RF_CONTROL_MATCHED || !rf_decode_report(frame, &report) ||
        frame->bytes[33] != 0x47) return false;
    unsigned target = s->mode == RF_OFF ? 320 : (18 * s->centi_c + 50) / 100 + 320;
    unsigned observed = (unsigned)frame->bytes[19] << 8 | frame->bytes[20];
    if (!report.target_available || observed != target || report.mode != s->mode) return false;
    s->phase = RF_CONTROL_MATCHED; s->matched_ms = now_ms; return true;
}

void rf_control_receive(rf_control *c, int64_t now_ms, const rf_frame *frame)
{
    rf_control_tick(c, now_ms);
    rf_report report;
    if (now_ms < c->started_ms || c->status.phase == RF_CONTROL_IDLE ||
        !rf_decode_report(frame, &report) || frame->bytes[33] != 0x47 ||
        memcmp(report.source, c->radiator, 4) || memcmp(report.destination, c->controller, 4)) return;
    // Late matching readback updates a result without restarting delivery.
    if (!rf_control_reconcile(&c->status, now_ms, frame) &&
        (c->status.phase == RF_CONTROL_WAIT_REPORT ||
         (c->status.phase == RF_CONTROL_WAIT_READBACK && c->status.attempts < RF_CONTROL_MAX_ATTEMPTS))) {
        if (frame->size > sizeof(c->report_bytes)) return;
        memcpy(c->report_bytes, frame->bytes, frame->size);
        c->report_size = frame->size;
        c->report_received_ms = now_ms; // Original receipt, never refreshed by next().
        c->status.phase = RF_CONTROL_SEND;
    }
}

const char *rf_control_phase_name(rf_control_phase phase)
{
    static const char *names[] = {"idle", "poll", "waiting_report", "sending",
                                 "waiting_readback", "matched", "timeout", "error"};
    return phase >= RF_CONTROL_IDLE && phase <= RF_CONTROL_ERROR ? names[phase] : "error";
}
