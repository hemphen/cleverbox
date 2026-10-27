// SPDX-License-Identifier: MIT
// Exercise the real admission and worker code with a deterministic fake platform.
#include "../radio_transport.cpp"
#include <cstdio>

extern "C" esp_err_t device_store_load(paired_device *) { return ESP_ERR_NOT_FOUND; }
extern "C" esp_err_t device_store_save(const paired_device *) { return ESP_OK; }

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
    control_pending = queue_full = false;
    dequeue_hook = transmit_hook = delay_hook = nullptr;
    transmit_result = 0; test_now_us = 1000000; checks = 0;
    radio_requests = xQueueCreate(1, sizeof(radio_request));
}

static void run_worker()
{
    try { receive_task(nullptr); assert(false); }
    catch (const stop_worker &) {}
}

int main()
{
    command.mode = RF_COMFORT; command.centi_c = 2050; command.clock_seconds = 43200;
    assert(rf_parse_hex("38F1010203C6F10405064500004B02A83B004C00008E000084CC00000000972030049402BA00E100641919DA000000005B0078B20C0000A20F60D9",
                        command.state.bytes, sizeof(command.state.bytes), &command.state.size));
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
