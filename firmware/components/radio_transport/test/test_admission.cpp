// SPDX-License-Identifier: MIT
#include "../radio_transport.cpp"
#include <cstdio>
extern "C" esp_err_t device_registry_load(radiator_registry *r) { *r = {}; r->next_id = 1; return ESP_OK; }
static int store_result;
extern "C" esp_err_t device_registry_save_record(const radiator_registry *, const radiator_record *, uint32_t) { return store_result; }
static rf_control_request command;
struct stop_worker {};
static unsigned checks;
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
    transmit_result = 0; test_now_us = 1000000; checks = 0;
    incoming_size = incoming_used = 0; radio_requests = xQueueCreate(1, sizeof(radio_request));
}
static void hear(radio_worker &worker, const rf_frame &frame) {
    memcpy(incoming_bytes, frame.bytes, frame.size); incoming_size = frame.size; incoming_used = 0;
    worker.step(); assert(incoming_used == incoming_size);
}
static void reject_second() {
    assert(radio_control_start(2, &command) == ESP_ERR_INVALID_STATE);
    uint32_t id; const uint8_t c[] = {0xf1,1,2,3}, candidate[] = {0xf1,0x10,0,1};
    assert(radio_pair_start(c, candidate, &id) == ESP_ERR_INVALID_STATE);
    assert(radio_device_remove(1) == ESP_ERR_INVALID_STATE);
    assert(!queue_full); ++checks;
}
int main() {
    command.mode = RF_COMFORT; command.centi_c = 2050; command.clock_seconds = 43200;
    reset(); radio_worker worker;
    assert(radio_control_start(0, &command) == ESP_ERR_INVALID_STATE); // Ambiguous.
    assert(radio_control_start(1, &command) == ESP_ERR_INVALID_STATE); // Missing state.
    rf_frame state = state_frame(), bad = state; bad.bytes[31] ^= 1; hear(worker, bad);
    assert(!registry.records[0].has_control_template);
    registry.records[0].pair.controller[3] ^= 1; hear(worker, state);
    assert(!registry.records[0].has_control_template); registry.records[0].pair.controller[3] ^= 1;
    hear(worker, state);
    assert(registry.records[0].has_control_template && !registry.records[1].has_control_template);
    assert(!registry.records[0].has_paired_report && registry.records[0].control.phase == RF_CONTROL_IDLE);
    assert(radio_state_save(1, nullptr) == ESP_OK);
    registry.records[0].control_template[31] ^= 1; // Admission owns the snapshot.
    worker.step();
    assert(registry.records[0].state_save_result == RADIO_STORE_OK);
    assert(!memcmp(registry.records[0].saved_template, state.bytes, state.size));
    registry.records[0].has_control_template = false;
    command.state = bad; assert(radio_control_start(1, &command) == ESP_ERR_INVALID_ARG);
    command.state = {}; assert(radio_control_start(1, &command) == ESP_OK);
    assert(radio_control_start(2, &command) != ESP_OK);
    assert(radio_device_remove(1) != ESP_OK);
    dequeue_hook = reject_second; transmit_hook = reject_second;
    worker.step(); assert(checks == 2); dequeue_hook = transmit_hook = nullptr;
    assert(registry.records[0].control.phase == RF_CONTROL_WAIT_REPORT);
    test_now_us += (RF_CONTROL_WAIT_MS + 1) * 1000LL; worker.step();
    assert(!exchange_id && registry.records[0].control.phase == RF_CONTROL_TIMEOUT);
    command.state = state;
    assert(radio_control_start(1, &command) == ESP_OK);
    transmit_result = -1; worker.step();
    assert(!exchange_id && registry.records[0].control.phase == RF_CONTROL_ERROR);
    transmit_result = 0;
    // Full shared queue releases only the failed command's reservation.
    assert(radio_device_rename(2, "Bedroom") == ESP_OK);
    assert(radio_control_start(1, &command) != ESP_OK);
    worker.step(); assert(!mutation_pending);
    // Explicit clear failure retains both fallback and learned state.
    hear(worker, state); store_result = ESP_ERR_INVALID_STATE;
    assert(radio_state_clear(1) == ESP_OK); worker.step();
    assert(registry.records[0].has_saved_template && registry.records[0].has_control_template);
    store_result = 0; assert(radio_state_clear(1) == ESP_OK); worker.step();
    assert(!registry.records[0].has_saved_template && !registry.records[0].has_control_template);
    // New admission during an empty worker iteration must survive publish.
    // Simulate exactly the state after another core enqueues the next operation.
    assert(radio_control_start(1, &command) == ESP_OK);
    worker.publish(); assert(exchange_id == 1 && exchange_pending);
    worker.step(); assert(exchange_id == 1 && !exchange_pending);
    test_now_us += (RF_CONTROL_WAIT_MS + 1) * 1000LL; worker.step();
    assert(registry.records[0].control.phase == RF_CONTROL_TIMEOUT);
    // A terminal attempted command can reconcile a late routed report while
    // B owns delivery; this updates only a result and never schedules A again.
    registry.records[0].control.attempts = 1;
    rf_frame bstate = {};
    assert(rf_parse_hex("38F1010203C6F10405074500004B02A83B004C00008E000084CC00000000977182939402BA00E100641919DA000000005BABCDB20C00000CFC7F4B", bstate.bytes, sizeof(bstate.bytes), &bstate.size));
    command.state = bstate; assert(radio_control_start(2, &command) == ESP_OK); worker.step();
    rf_frame matching = {};
    assert(rf_parse_hex("23F1040506C6F1010203DF203000404B02A88A02B100E100641919DA000A000A0A47C783C11D", matching.bytes, sizeof(matching.bytes), &matching.size));
    hear(worker, matching);
    assert(registry.records[0].control.phase == RF_CONTROL_MATCHED);
    assert(registry.records[1].control.phase == RF_CONTROL_WAIT_REPORT && exchange_id == 2);
    test_now_us += (RF_CONTROL_WAIT_MS + 1) * 1000LL; worker.step(); command.state = state;
    // Pair admission has the same global reservation, including queue/worker handoff.
    const uint8_t c[] = {0xf1,1,2,3}, candidate[] = {0xf1,0x10,0,1}; uint32_t id;
    assert(radio_pair_start(c, candidate, &id) == ESP_OK && id == 3);
    worker.publish(); assert(exchange_id == id && exchange_pending);
    assert(radio_control_start(1, &command) != ESP_OK);
    worker.step(); assert(pair_reserved && exchange_id == id);
    assert(radio_pair_cancel(id + 1) != ESP_OK);
    assert(radio_pair_cancel(id) == ESP_OK); worker.step(); assert(!exchange_id);
    // Queue-full enqueue failure cannot leave a reservation behind.
    queue_full = true; assert(radio_control_start(1, &command) == ESP_ERR_TIMEOUT);
    assert(!exchange_id && !exchange_pending); queue_full = false;
    printf("Memory: capacity=%u record=%zu registry=%zu worker=%zu request=%zu\n", (unsigned)CLEVERBOX_RADIATOR_CAPACITY, sizeof(radiator_record), sizeof(registry), sizeof(radio_worker), sizeof(radio_request));
    puts("Identity ownership, learning, snapshot/save/clear, global reservations across handoff/TX, timeout, queue/storage failures: passed");
}
