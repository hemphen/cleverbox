// SPDX-License-Identifier: MIT
#include "../radio_transport.cpp"
#include <cstdio>
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
static void reset() {
    status = {}; status.ready = true; registry = {}; registry.count = 2; registry.next_id = 3;
    for (unsigned i = 0; i < 2; ++i) {
        auto &d = registry.records[i]; d.id = i + 1;
        const uint8_t controller[] = {0xf1,1,2,3}, radiator[] = {0xf1,4,5,6};
        memcpy(d.pair.controller, controller, 4); memcpy(d.pair.radiator, radiator, 4); d.pair.radiator[3] += i;
    }
    mutation_pending = exchange_pending = pair_reserved = queue_full = false; exchange_id = 0;
    dequeue_hook = transmit_hook = delay_hook = nullptr; store_result = 0;
    transmit_result = receive_result = 0; transmit_count = 0; test_now_us = 1000000; checks = 0;
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
    printf("Memory: capacity=%u record=%zu registry=%zu worker=%zu request=%zu\n", (unsigned)CLEVERBOX_RADIATOR_CAPACITY, sizeof(radiator_record), sizeof(registry), sizeof(radio_worker), sizeof(radio_request));
    puts("Independent reservations, fair turns, compaction, fatal radio failure, handoff and queue capacity: passed");
}

extern "C" bool device_time_command_clock(unsigned *, int64_t *) { return false; }
