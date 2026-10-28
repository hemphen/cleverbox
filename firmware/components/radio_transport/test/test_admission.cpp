// SPDX-License-Identifier: MIT
// Exercise the real admission and worker code with a deterministic fake platform.
#include "../radio_transport.cpp"
#include <cstdio>

extern "C" esp_err_t device_store_load(paired_device *) { return ESP_ERR_NOT_FOUND; }
extern "C" esp_err_t device_store_save(const paired_device *) { return ESP_OK; }

extern "C" esp_err_t device_store_load_state(const paired_device *, rf_frame *) { return ESP_ERR_NOT_FOUND; }
extern "C" esp_err_t device_store_save_state(const paired_device *, const rf_frame *) { return ESP_OK; }

static rf_control_request command;
struct stop_worker {};
static unsigned checks;

static void reject_second()
{
    assert(radio_control_start(&command) == ESP_ERR_INVALID_STATE);
    assert(!queue_full);
    ++checks;
}

static void reset()
{
    status = {}; status.ready = true; status.pairing_saved = true;
    status.pairing.phase = RF_PAIR_COMPLETE;
    const uint8_t controller[4] = {0xf1,1,2,3}, radiator[4] = {0xf1,4,5,6};
    memcpy(status.pairing.controller, controller, 4);
    memcpy(status.pairing.radiator, radiator, 4);
    control_pending = state_store_pending = queue_full = false;
    dequeue_hook = transmit_hook = delay_hook = nullptr;
    transmit_result = 0; test_now_us = 1000000; checks = 0;
    incoming_size = incoming_used = 0;
    radio_requests = xQueueCreate(1, sizeof(radio_request));
}

static void run_worker()
{
    try { receive_task(nullptr); assert(false); }
    catch (const stop_worker &) {}
}

static rf_frame state_frame()
{
    rf_frame state = {};
    assert(rf_parse_hex("38F1010203C6F10405064500004B02A83B004C00008E000084CC00000000972030049402BA00E100641919DA000000005B0078B20C0000A20F60D9",
                        state.bytes, sizeof(state.bytes), &state.size));
    return state;
}

static void hear(const rf_frame &frame)
{
    assert(frame.size <= sizeof(incoming_bytes));
    memcpy(incoming_bytes, frame.bytes, frame.size);
    incoming_size = frame.size; incoming_used = 0;
    delay_hook = [] { throw stop_worker{}; };
    transmit_hook = [] { assert(false); }; // Learning alone never transmits.
    run_worker();
    assert(incoming_used == incoming_size);
}

int main()
{
    command.mode = RF_COMFORT; command.centi_c = 2050; command.clock_seconds = 43200;
    command.state = {};
    reset();
    assert(radio_control_start(&command) == ESP_ERR_INVALID_STATE); // No implicit zero/default state.
    assert(!queue_full && !control_pending);
    rf_frame state = state_frame();
    status.pairing_saved = false; hear(state);
    assert(!status.has_control_template);
    status.pairing_saved = true;
    status.pairing.controller[3] ^= 1; hear(state);
    assert(!status.has_control_template);
    status.pairing.controller[3] ^= 1;
    status.pairing.radiator[3] ^= 1; hear(state);
    assert(!status.has_control_template);
    status.pairing.radiator[3] ^= 1;
    rf_frame bad = state; bad.bytes[31] ^= 1; hear(bad);
    assert(!status.has_control_template);
    rf_frame malformed = {};
    // Independently sealed by reference decoder.py, but tag 98 replaces 97.
    assert(rf_parse_hex("38F1010203C6F10405064500004B02A83B004C00008E000084CC00000000982030049402BA00E100641919DA000000005B0078B20C0000A22BA00E",
                        malformed.bytes, sizeof(malformed.bytes), &malformed.size));
    assert(rf_valid(&malformed)); hear(malformed);
    assert(!status.has_control_template);
    rf_frame poll = {}; assert(rf_short(&poll, status.pairing.controller, status.pairing.radiator, RF_POLL));
    hear(poll); assert(!status.has_control_template); // Valid envelope, wrong application shape.
    hear(state);
    assert(status.has_control_template && status.control_template_time_us == test_now_us);
    assert(!memcmp(status.control_template.bytes, state.bytes, state.size));
    assert(status.control.phase == RF_CONTROL_IDLE && !status.has_paired_report);
    // Saving also owns its admission snapshot, and never asks the radio to TX.
    assert(radio_state_save(nullptr) == ESP_OK);
    assert(status.state_save_result == RADIO_STORE_PENDING);
    rf_frame original = status.control_template;
    status.control_template.bytes[31] ^= 1;
    radio_worker save_worker;
    save_worker.step();
    assert(status.state_save_result == RADIO_STORE_OK && status.has_saved_template);
    assert(!memcmp(status.saved_template.bytes, original.bytes, original.size));
    status.control_template = original;
    command.state = bad;
    assert(radio_control_start(&command) == ESP_ERR_INVALID_ARG); // Never hide a bad import with learned state.
    command.state = {};
    bad = state; bad.bytes[55] ^= 1; hear(bad);
    hear(poll); // Neither invalid traffic nor another shape overwrites the cache.
    assert(!memcmp(status.control_template.bytes, state.bytes, state.size));
    assert(radio_control_start(&command) == ESP_OK);
    status.control_template.bytes[31] ^= 1; // Queued command owns its snapshot.
    radio_request learned;
    assert(xQueueReceive(radio_requests, &learned, 0) == pdPASS);
    assert(learned.control.state.size == state.size &&
           !memcmp(learned.control.state.bytes, state.bytes, state.size));
    reset(); // Reboot loses the learned state and never schedules a command.
    assert(!status.has_control_template && radio_control_start(&command) == ESP_ERR_INVALID_STATE);
    command.state = bad;
    assert(radio_control_start(&command) == ESP_ERR_INVALID_ARG); // Bad explicit import cannot fall back.
    command.state = state;
    reset();
    assert(radio_control_start(&command) == ESP_OK);
    assert(radio_control_start(&command) != ESP_OK); // Still queued.
    dequeue_hook = reject_second; // Queue empty, active status not yet published.
    transmit_hook = reject_second; // Reservation handed over to active status.
    delay_hook = [] {
        if (status.control.phase == RF_CONTROL_WAIT_REPORT) {
            reject_second();
            test_now_us += (RF_CONTROL_WAIT_MS + 1) * 1000LL;
        } else {
            assert(status.control.phase == RF_CONTROL_TIMEOUT);
            assert(radio_control_start(&command) == ESP_OK);
            throw stop_worker{};
        }
    };
    run_worker();
    assert(checks == 3);

    reset();
    assert(radio_pair_cancel() == ESP_OK); // Occupy the shared queue.
    assert(radio_control_start(&command) == ESP_ERR_TIMEOUT);
    assert(!control_pending);
    radio_request discarded;
    assert(xQueueReceive(radio_requests, &discarded, 0) == pdPASS);
    assert(radio_control_start(&command) == ESP_OK); // Failed enqueue released the slot.
    transmit_result = -1;
    delay_hook = [] {
        assert(status.control.phase == RF_CONTROL_ERROR);
        assert(radio_control_start(&command) == ESP_OK); // TX error also allows retry.
        throw stop_worker{};
    };
    run_worker();
    puts("Command admission across dequeue, active delivery, timeout, queue/TX failure: passed");
}
