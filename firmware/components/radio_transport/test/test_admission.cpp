// SPDX-License-Identifier: MIT
#include "../radio_transport.cpp"
#include <cstdio>
#include <initializer_list>
extern "C" esp_err_t device_registry_load(radiator_registry *r) { *r = {}; r->next_id = 1; return ESP_OK; }
static int store_result;
extern "C" esp_err_t device_registry_save_record(const radiator_registry *, const radiator_record *, uint32_t) { return store_result; }
static rf_control_request command;
struct stop_worker {};
static unsigned checks;
static rf_frame b_state;
static rf_frame state_frame() {
    rf_frame f = {};
    assert(rf_parse_hex("38F1010203C6F10405064500004B02A83B004C00008E000084CC00000000972030049402BA00E100641919DA000000005B0078B20C0000A20F60D9", f.bytes, sizeof(f.bytes), &f.size)); return f;
}
extern "C" esp_err_t device_mode_load(bool *enabled, bool *stored) { *enabled = true; *stored = true; return ESP_OK; }
extern "C" esp_err_t device_mode_start(bool *enabled) { *enabled = true; return ESP_OK; }
static int mode_store_result;
extern "C" esp_err_t device_mode_save(bool) { return mode_store_result; }
static void reset() {
    status_lock = xSemaphoreCreateMutex(); mode_store_result = 0; tx_in_progress = false;
    capture_sequence = 0; capture_boot = 1;
    status = {}; status.ready = true; status.control_enabled = true; registry = {}; registry.count = 2; registry.next_id = 3;
    for (unsigned i = 0; i < 2; ++i) {
        auto &d = registry.records[i]; d.id = i + 1;
        const uint8_t controller[] = {0xf1,1,2,3}, radiator[] = {0xf1,4,5,6};
        memcpy(d.pair.controller, controller, 4); memcpy(d.pair.radiator, radiator, 4); d.pair.radiator[3] += i;
    }
    mutation_pending = exchange_pending = pair_reserved = queue_full = false; exchange_id = 0;
    dequeue_hook = transmit_hook = delay_hook = nullptr; store_result = 0;
    transmit_result = receive_result = 0; transmit_count = 0; test_now_us = 1000000; checks = 0;
    count_reads = receive_starts = 0; count_hook = nullptr; receiving = true; packet_mode = 2; fixed_length = 0;
    incoming_size = incoming_used = 0; radio_requests = xQueueCreate(CLEVERBOX_RADIATOR_CAPACITY + 1, sizeof(radio_request));
}
static void hear(radio_worker &worker, const rf_frame &frame) {
    memcpy(incoming_bytes, frame.bytes, frame.size); incoming_size = frame.size; incoming_used = 0;
    worker.step(); assert(incoming_used == incoming_size);
}
static void during_dequeue() {
    assert(radio_control_start(1, &command) == ESP_ERR_INVALID_STATE);
    command.state = b_state;
    assert(radio_control_start(2, &command) == ESP_OK); // Admission during A handoff.
    ++checks;
}
static rf_frame report_b(bool matching) {
    rf_frame f = {};
    const char *hex = matching
        ? "23F1040507C6F1010203DF203000404B02A88A02B100E100641919DA000A000A0A47C6D3D4FD"
        : "23F1040507C6F1010203DF203000404B02A88A02BA00E100641919DA000A000A0A47CC18D07D";
    assert(rf_parse_hex(hex, f.bytes, sizeof(f.bytes), &f.size) && rf_valid(&f));
    return f;
}
static void test_queue_capacity() {
    reset(); radio_request in = {}, out = {};
    for (unsigned i = 0; i < CLEVERBOX_RADIATOR_CAPACITY + 1; ++i) {
        in.id = i + 1; assert(xQueueSend(radio_requests, &in, 0) == pdPASS);
    }
    assert(xQueueSend(radio_requests, &in, 0) != pdPASS);
    for (unsigned i = 0; i < CLEVERBOX_RADIATOR_CAPACITY + 1; ++i) {
        assert(xQueueReceive(radio_requests, &out, 0) == pdPASS && out.id == i + 1);
    }
    assert(!queue_count);
}
static void test_fair_turns() {
    reset(); radio_worker worker;
    rf_control_request a = command, b = command;
    a.state = state_frame(); b.state = b_state;
    assert(rf_control_start(&worker.controls[0], &a, registry.records[0].pair.controller,
                            registry.records[0].pair.radiator, test_now_us / 1000));
    assert(rf_control_start(&worker.controls[1], &b, registry.records[1].pair.controller,
                            registry.records[1].pair.radiator, test_now_us / 1000));
    registry.records[0].command_reserved = registry.records[1].command_reserved = true;
    for (unsigned i = 0; i < 6; ++i) {
        // Both devices remain ready at every turn; exactly one frame is sent per step.
        worker.controls[0].status.phase = worker.controls[1].status.phase = RF_CONTROL_POLL;
        worker.step();
        assert(transmit_count == i + 1 && transmitted_size == 16);
        assert(transmitted_bytes[9] == 6 + i % 2);
    }
}
static void test_compaction() {
    reset(); radio_worker worker; command.state = b_state;
    assert(radio_control_start(2, &command) == ESP_OK); worker.step();
    assert(registry.records[1].control.phase == RF_CONTROL_WAIT_REPORT);
    assert(radio_device_remove(1) == ESP_OK); worker.step();
    assert(registry.count == 1 && registry.records[0].id == 2);
    assert(registry.records[0].command_reserved && worker.controls[0].radiator[3] == 7);
    hear(worker, report_b(false)); worker.step();
    assert(registry.records[0].control.phase == RF_CONTROL_WAIT_READBACK);
    hear(worker, report_b(true));
    assert(registry.records[0].control.phase == RF_CONTROL_MATCHED);
    assert(!registry.records[0].command_reserved);
}
static void test_fatal_rx_failure() {
    reset(); radio_worker worker;
    registry.count = 3; registry.next_id = 4;
    registry.records[2] = registry.records[0]; registry.records[2].id = 3;
    registry.records[2].pair.radiator[3] = 8;
    rf_frame cstate = {};
    assert(rf_parse_hex("38F1010203C6F10405084500004B02A83B004C00008E000084CC00000000972030049402BA00E100641919DA000000005B0078B20C000045403973",
                        cstate.bytes, sizeof(cstate.bytes), &cstate.size) && rf_valid(&cstate));
    command.state = state_frame();
    assert(radio_control_start(1, &command) == ESP_OK); worker.step();
    assert(registry.records[0].control.phase == RF_CONTROL_WAIT_REPORT);
    command.state = b_state; assert(radio_control_start(2, &command) == ESP_OK);
    command.state = cstate; assert(radio_control_start(3, &command) == ESP_OK);
    assert(queue_count == 2 && registry.records[2].command_pending);
    receive_result = -77; worker.step(); // B transmits; restoring RX fails while C is still queued.
    assert(!status.ready && status.error == -77 && queue_count == 1);
    for (unsigned i = 0; i < 3; ++i) {
        auto &d = registry.records[i];
        assert(d.control.phase == RF_CONTROL_ERROR && d.control_error == -77);
        assert(!d.command_reserved && !d.command_pending);
    }
    assert(radio_control_start(1, &command) == ESP_ERR_INVALID_STATE);
}
static void test_admission_clock_and_queue_delay() {
    reset(); radio_worker worker;
    command.state = b_state; command.clock_seconds = 86399;
    assert(radio_control_start_device(2, &command) == RADIO_ERR_TIME_UNAVAILABLE);
    assert(!queue_count && !registry.records[1].command_reserved);
    assert(radio_control_start(2, &command) == ESP_OK);
    test_now_us += 7300000; worker.step();
    assert(worker.controls[1].started_ms == 1000);
    assert(worker.controls[1].deadline_ms == test_now_us / 1000 + RF_CONTROL_WAIT_MS);
    test_now_us += 3200000; hear(worker, report_b(false)); worker.step();
    assert(transmitted_size == 59 && transmitted_bytes[52] == 0 && transmitted_bytes[53] == 0 && transmitted_bytes[54] == 9);
    assert(worker.controls[1].status.attempts == 1);
    test_now_us += 5000000; hear(worker, report_b(false)); worker.step();
    assert(transmitted_bytes[54] == 14 && worker.controls[1].status.attempts == 2);
    command.clock_seconds = 43200;
}
// FIFO/count/time injection at the existing RadioLib boundary. The production
// worker and protocol machines run here; these are not physical RF timing tests.
static void load_fifo(const rf_frame &f, size_t size) {
    assert(size <= f.size && size <= sizeof(incoming_bytes));
    memcpy(incoming_bytes, f.bytes, size); incoming_size = size; incoming_used = 0;
}
static void start_control(radio_worker &worker, unsigned index) {
    auto &d = registry.records[index];
    rf_control_request r = command; r.state = index ? b_state : state_frame();
    assert(rf_control_start(&worker.controls[index], &r, d.pair.controller, d.pair.radiator, test_now_us / 1000));
    d.command_reserved = true;
}
static void test_partial_and_fifo_only() {
    for (size_t prefix : {size_t(1), size_t(8)}) {
        reset(); radio_worker worker; rf_frame report = report_b(false);
        load_fifo(report, prefix);
        worker.step(); // No outgoing work yet; leave the last live FIFO byte.
        assert(incoming_size - incoming_used == 1);
        assert(worker.frame.size == prefix - 1);
        command.state = state_frame();
        assert(radio_control_start(1, &command) == ESP_OK);
        worker.step(); // A's poll is ready while B's report is still incomplete.
        assert(!transmit_count && !receive_starts && incoming_size - incoming_used == 1);
        assert(worker.controls[0].status.phase == RF_CONTROL_POLL && worker.next_device == 0);
        assert(!worker.controls[0].status.attempts);
        // Append without replacing the byte left in the hardware FIFO.
        memcpy(incoming_bytes + prefix, report.bytes + prefix, report.size - prefix);
        incoming_size = report.size;
        worker.step();
        assert(status.valid_frames == 1 && registry.records[1].has_paired_report);
        assert(!registry.records[0].has_paired_report);
        assert(transmit_count == 1 && transmitted_bytes[9] == 6);
        assert(registry.records[0].control.phase == RF_CONTROL_WAIT_REPORT);
        worker.step(); assert(transmit_count == 1); // No lost or duplicated poll.
    }
    // FIFO-only bytes already present when outgoing work is handed off.
    reset(); radio_worker worker; load_fifo(report_b(false), 1);
    command.state = state_frame(); assert(radio_control_start(1, &command) == ESP_OK);
    worker.step(); assert(!transmit_count && !worker.frame.size && incoming_used == 0);
}
static void test_report_before_selection() {
    // A pre-poll report is observed before TX; it cannot satisfy the fresh-report
    // requirement by being processed after that poll in the same iteration.
    reset(); radio_worker pre_poll; start_control(pre_poll, 1);
    load_fifo(report_b(false), report_b(false).size); pre_poll.step();
    assert(status.valid_frames == 1 && registry.records[1].has_paired_report);
    assert(transmit_count == 1 && pre_poll.controls[1].status.phase == RF_CONTROL_WAIT_REPORT);
    assert(!pre_poll.controls[1].report_size && !pre_poll.controls[1].status.attempts);
    reset(); radio_worker worker; start_control(worker, 1);
    worker.step(); // B poll.
    hear(worker, report_b(false)); // B state, following the report in this step.
    assert(transmit_count == 2 && worker.controls[1].status.attempts == 1);
    // Make B's retry ready, but put matching readback in FIFO before selection.
    rf_frame mismatch = report_b(false);
    rf_control_receive(&worker.controls[1], test_now_us / 1000, &mismatch);
    assert(worker.controls[1].status.phase == RF_CONTROL_SEND);
    start_control(worker, 0); worker.next_device = 1;
    load_fifo(report_b(true), report_b(true).size); worker.step();
    assert(registry.records[1].control.phase == RF_CONTROL_MATCHED);
    assert(!registry.records[1].command_reserved && worker.controls[1].status.attempts == 1);
    assert(transmit_count == 3 && transmitted_size == 16 && transmitted_bytes[9] == 6);
}
static uint8_t unstable_count(uint8_t) { return count_reads % 2; }
static uint8_t overflow_count(uint8_t) { return 0x80; }
static uint8_t impossible_count(uint8_t) { return 65; }
static uint8_t late_fifo(uint8_t count) {
    // First service finds idle; bytes arrive only at the final guard.
    if (count_reads == 3) load_fifo(report_b(false), 1);
    return count_reads >= 3 ? incoming_size - incoming_used : count;
}
static uint8_t late_unstable(uint8_t count) { return count_reads > 2 ? count_reads % 2 : count; }
static uint8_t crossing_deadline(uint8_t count) {
    if (count_reads == 3) test_now_us += RF_CONTROL_WAIT_MS * 1000LL;
    return count;
}
static void test_final_guard_and_expiry() {
    for (auto hook : {late_fifo, late_unstable}) {
        reset(); radio_worker worker; start_control(worker, 1); count_hook = hook;
        worker.step();
        assert(!transmit_count && worker.controls[1].status.phase == RF_CONTROL_POLL);
        assert(!worker.controls[1].status.attempts && worker.next_device == 0);
        assert(count_reads <= 11);
        count_hook = nullptr; test_now_us += 100001; worker.step();
        assert(status.receive_errors == 1 && transmit_count == 1);
        worker.step(); assert(transmit_count == 1);
    }
    reset(); radio_worker worker; start_control(worker, 1); count_hook = crossing_deadline;
    worker.step();
    assert(!transmit_count && registry.records[1].control.phase == RF_CONTROL_TIMEOUT);
    assert(!registry.records[1].command_reserved);
}
static void test_bounded_rx_recovery() {
    for (auto hook : {unstable_count, overflow_count, impossible_count}) {
        reset(); radio_worker worker; start_control(worker, 0); count_hook = hook;
        worker.step(); assert(!transmit_count && count_reads <= 11);
        assert(status.receive_errors == (hook == unstable_count ? 0 : 1));
        count_reads = 0; test_now_us += 100001; worker.step();
        assert(!transmit_count && status.receive_errors >= 1 && count_reads <= 18);
        count_hook = nullptr; test_now_us += 100001; worker.step();
        assert(transmit_count == 1 && status.ready);
    }
    reset(); radio_worker worker; start_control(worker, 0);
    load_fifo(report_b(false), 8); worker.step();
    test_now_us += 100000; worker.step(); assert(!transmit_count); // Existing >100 ms rule.
    test_now_us += 1; worker.step();
    assert(status.receive_errors == 1 && transmit_count == 1 && !worker.frame.size && receiving);
    // Zero is a valid monotonic timestamp for starting the bounded RX window.
    reset(); radio_worker zero; test_now_us = 0; load_fifo(report_b(false), 1); zero.step();
    test_now_us = 100001; zero.step(); assert(status.receive_errors == 1);
}
static void test_deferred_state_and_results() {
    reset(); radio_worker worker; start_control(worker, 1); worker.step();
    rf_frame report = report_b(false);
    rf_control_receive(&worker.controls[1], test_now_us / 1000, &report);
    const auto deadline = worker.controls[1].deadline_ms;
    const auto turn = worker.next_device;
    load_fifo(report_b(false), 1);
    for (unsigned i = 0; i < 10; ++i) {
        test_now_us += 1000; worker.step();
        assert(transmit_count == 1 && !worker.controls[1].status.attempts);
        assert(worker.controls[1].deadline_ms == deadline && worker.next_device == turn);
    }
    incoming_size = incoming_used = 0; test_now_us += 100001; worker.step();
    assert(transmit_count == 2 && transmitted_size == 59 && worker.controls[1].status.attempts == 1);
    worker.step(); assert(transmit_count == 2);
    hear(worker, report_b(true));
    assert(registry.records[1].control.phase == RF_CONTROL_MATCHED && !registry.records[1].command_reserved);
    // A TX error is still a real attempt, and reception is restored.
    reset(); radio_worker failed; start_control(failed, 1); failed.step();
    rf_control_receive(&failed.controls[1], test_now_us / 1000, &report);
    transmit_result = -99; failed.step();
    assert(transmit_count == 2 && status.ready && receiving && packet_mode == 2);
    assert(registry.records[1].control.phase == RF_CONTROL_ERROR && registry.records[1].control.attempts == 1);
    assert(registry.records[1].control_error == -99);
    hear(failed, report_b(true)); assert(registry.records[1].control.phase == RF_CONTROL_MATCHED);
}
static void test_expired_handoffs_and_rx_failure() {
    reset(); radio_worker command_worker; command.state = state_frame();
    assert(radio_control_start(1, &command) == ESP_OK);
    test_now_us += RF_CONTROL_WAIT_MS * 1000LL; command_worker.step();
    assert(!transmit_count && registry.records[0].control.phase == RF_CONTROL_TIMEOUT);
    assert(!registry.records[0].command_reserved);
    reset(); radio_worker pairing; uint32_t id;
    const uint8_t c[] = {0xf1,1,2,3}, candidate[] = {0xf1,0x10,0,1};
    assert(radio_pair_start(c, candidate, &id) == ESP_OK);
    test_now_us += 5000000; pairing.step();
    assert(!transmit_count && status.pairing.phase == RF_PAIR_TIMEOUT && !pair_reserved);
    reset(); radio_worker broken; start_control(broken, 0);
    count_hook = overflow_count; receive_result = -77; broken.step();
    assert(!transmit_count && !status.ready && status.error == -77);
    assert(registry.records[0].control.phase == RF_CONTROL_ERROR && !registry.records[0].command_reserved);
    assert(!registry.records[0].control.attempts && registry.records[0].control_error == -77);
}
static void test_sustained_input_deadlines() {
    // A is waiting for a report; B has a ready state retry. Sustained/uncertain
    // input cannot reset either deadline, consume attempts or freeze the owner.
    reset(); radio_worker worker; start_control(worker, 0); start_control(worker, 1);
    worker.step(); worker.step(); rf_frame report = report_b(false);
    rf_control_receive(&worker.controls[1], test_now_us / 1000, &report);
    const int64_t deadline = worker.controls[1].deadline_ms;
    count_hook = unstable_count;
    for (int64_t ms = test_now_us / 1000; ms <= deadline; ms += 101) {
        test_now_us = ms * 1000; count_reads = 0; worker.step(); assert(count_reads <= 18);
    }
    test_now_us = deadline * 1000; worker.step();
    for (unsigned i = 0; i < 2; ++i) {
        assert(registry.records[i].control.phase == RF_CONTROL_TIMEOUT);
        assert(!registry.records[i].command_reserved && !registry.records[i].control.attempts);
    }
    assert(transmit_count == 2 && status.ready && status.receive_errors > 100);
    count_hook = nullptr; worker.step(); assert(transmit_count == 2);
    // Offer is bounded from admission, including queue delay; bind retains the
    // offer's reply deadline. No offer/bind retries or synthetic sent callbacks.
    for (bool bind : {false, true}) {
        reset(); registry.count = 0; radio_worker pairing; const uint8_t c[] = {0xf1,1,2,3}, candidate[] = {0xf1,0x10,0,1}; uint32_t id;
        assert(radio_pair_start(c, candidate, &id) == ESP_OK);
        if (bind) {
            pairing.step(); assert(transmit_count == 1);
            rf_frame reply = {}; assert(rf_parse_hex("0DF1040506C6F1010203000080E1D2A1", reply.bytes, sizeof(reply.bytes), &reply.size));
            // Use an independent valid reply for an unconfigured address.
            assert(rf_valid(&reply)); hear(pairing, reply); assert(pairing.pair.phase == RF_PAIR_BIND);
        }
        count_hook = unstable_count;
        const int64_t limit = test_now_us + 5000000;
        while (test_now_us <= limit) {
            count_reads = 0; pairing.step(); assert(count_reads <= 18); test_now_us += 101000;
        }
        pairing.step();
        assert(status.pairing.phase == RF_PAIR_TIMEOUT && !pair_reserved && !exchange_id);
        assert(transmit_count == unsigned(bind) && status.ready);
    }
    reset(); radio_worker malformed; start_control(malformed, 1);
    // Invalid packets are discarded; they do not prevent progress on an idle gap.
    rf_frame bad = report_b(false); bad.bytes[16] ^= 1; load_fifo(bad, bad.size);
    malformed.step(); assert(status.invalid_frames == 1 && transmit_count == 1 && status.ready);
}
#include "passive_cases.inc"
int main() {
    command.mode = RF_COMFORT; command.centi_c = 2050; command.clock_seconds = 43200;
    reset(); radio_worker worker;
    rf_frame state = state_frame();
    assert(radio_control_start(0, &command) == ESP_ERR_INVALID_STATE);
    assert(radio_control_start(1, &command) == ESP_ERR_INVALID_STATE);
    hear(worker, state);
    assert(registry.records[0].has_control_template && !registry.records[1].has_control_template);
    rf_frame bstate = state;
    const char *bhex = "38F1010203C6F10405074500004B02A83B004C00008E000084CC00000000977182939402BA00E100641919DA000000005BABCDB20C00000CFC7F4B";
    assert(rf_parse_hex(bhex, bstate.bytes, sizeof(bstate.bytes), &bstate.size));
    hear(worker, bstate); b_state = bstate;
    command.state = {};
    assert(radio_control_start(1, &command) == ESP_OK);
    assert(radio_control_start(1, &command) == ESP_ERR_INVALID_STATE);
    assert(radio_device_remove(1) == ESP_ERR_INVALID_STATE);
    uint32_t id; const uint8_t c[] = {0xf1,1,2,3}, candidate[] = {0xf1,0x10,0,1};
    assert(radio_pair_start(c, candidate, &id) == ESP_ERR_INVALID_STATE);
    dequeue_hook = during_dequeue;
    worker.publish(); assert(registry.records[0].command_reserved && registry.records[0].command_pending);
    worker.step(); assert(checks == 1 && registry.records[0].control.phase == RF_CONTROL_WAIT_REPORT);
    dequeue_hook = nullptr; command.state = {};
    assert(registry.records[1].command_reserved && registry.records[1].command_pending);
    assert(radio_control_start(2, &command) == ESP_ERR_INVALID_STATE);
    // B was admitted during A handoff; one transmit per step.
    worker.step();
    assert(registry.records[0].control.phase == RF_CONTROL_WAIT_REPORT);
    assert(registry.records[1].control.phase == RF_CONTROL_WAIT_REPORT);
    hear(worker, report_b(false)); worker.step();
    assert(registry.records[1].control.phase == RF_CONTROL_WAIT_READBACK);
    hear(worker, report_b(true));
    assert(registry.records[0].control.phase == RF_CONTROL_WAIT_REPORT);
    assert(registry.records[1].control.phase == RF_CONTROL_MATCHED);
    test_now_us += (RF_CONTROL_WAIT_MS + 1) * 1000LL; worker.step();
    assert(registry.records[0].control.phase == RF_CONTROL_TIMEOUT);
    assert(registry.records[1].control.phase == RF_CONTROL_MATCHED);
    assert(!registry.records[0].command_reserved && !registry.records[1].command_reserved);
    assert(radio_pair_start(c, candidate, &id) == ESP_OK);
    assert(radio_control_start(1, &command) == ESP_ERR_INVALID_STATE);
    worker.step(); assert(pair_reserved && exchange_id == id);
    assert(radio_pair_cancel(id) == ESP_OK); worker.step(); assert(!exchange_id);
    queue_full = true; assert(radio_control_start(1, &command) == ESP_ERR_TIMEOUT);
    assert(!registry.records[0].command_reserved); queue_full = false;
    test_queue_capacity(); test_fair_turns(); test_compaction(); test_fatal_rx_failure(); test_admission_clock_and_queue_delay();
    test_partial_and_fifo_only(); test_report_before_selection(); test_final_guard_and_expiry();
    test_bounded_rx_recovery(); test_deferred_state_and_results();
    test_expired_handoffs_and_rx_failure(); test_sustained_input_deadlines();
    test_passive_mode(); test_passive_capture();
    reset(); registry.count=0; monitors={}; radio_worker monitoring;
    status.control_enabled=false; hear(monitoring,report_b(false));
    assert(radio_monitor_change("select",report_b(false).bytes+1,"Observed")==ESP_OK);
    monitoring.fail(ESP_ERR_TIMEOUT);
    assert(!status.monitor_pending && status.monitor_result==RADIO_STORE_FAILED && status.monitor_error==ESP_ERR_TIMEOUT);
    assert(!monitors.count && !transmit_count);
    puts("RX-before-TX arbitration, FIFO errata, late guard, bounded recovery/deadlines and deferred work: passed");
    printf("Memory: capacity=%u record=%zu registry=%zu worker=%zu request=%zu\n", (unsigned)CLEVERBOX_RADIATOR_CAPACITY, sizeof(radiator_record), sizeof(registry), sizeof(radio_worker), sizeof(radio_request));
    puts("Independent reservations, fair turns, compaction, fatal radio failure, handoff and queue capacity: passed");
}

extern "C" bool device_time_command_clock(unsigned *, int64_t *) { return false; }

extern "C" esp_err_t device_monitors_load(monitor_registry *r) { *r={}; return ESP_OK; }
extern "C" esp_err_t device_monitors_save(const monitor_registry *) { return ESP_OK; }
