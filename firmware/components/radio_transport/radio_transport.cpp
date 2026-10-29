// SPDX-License-Identifier: MIT
#ifdef CLEVERBOX_HOST
#include "platform.h"
#else
#include "idf_hal.h"
#endif
#include "radio_transport.h"
#include "esp_log.h"
#include "device_store.h"
#include "freertos/queue.h"
#include <string.h>

static IdfHal hal;
static Module module(&hal, RADIO_CS, RADIO_GDO0, RADIOLIB_NC, RADIO_GDO2);
static CC1101 radio(&module);
static radio_status status = {};
static portMUX_TYPE status_lock = portMUX_INITIALIZER_UNLOCKED;

extern "C" radio_status radio_transport_status(void)
{
    portENTER_CRITICAL(&status_lock);
    radio_status copy = status;
    portEXIT_CRITICAL(&status_lock);
    return copy;
}

static radiator_registry registry;
static bool mutation_pending;
static uint32_t exchange_id; // Admission reservation remains held across dequeue/TX.
static bool pair_reserved;
static bool exchange_pending;
static radiator_record *find_device(uint32_t id) {
    if (!id && registry.count == 1) id = registry.records[0].id;
    for (size_t i = 0; i < registry.count; ++i)
        if (id && registry.records[i].id == id) return &registry.records[i];
    return nullptr;
}
extern "C" esp_err_t radio_device_status(uint32_t id, radiator_record *out) {
    portENTER_CRITICAL(&status_lock);
    radiator_record *d = find_device(id);
    if (d) *out = *d;
    portEXIT_CRITICAL(&status_lock);
    return d ? ESP_OK : ESP_ERR_NOT_FOUND;
}
extern "C" esp_err_t radio_device_at(size_t index, radiator_record *out) {
    portENTER_CRITICAL(&status_lock);
    bool found = index < registry.count;
    if (found) *out = registry.records[index];
    portEXIT_CRITICAL(&status_lock);
    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}
enum radio_action { PAIR_START, PAIR_CANCEL, PAIR_SAVE, CONTROL_START, STATE_SAVE, STATE_CLEAR, RENAME, REMOVE };
struct radio_request {
    radio_action action; uint32_t id;
    uint8_t controller[4], candidate[4];
    rf_control_request control;
    char name[RADIATOR_NAME_BYTES + 1];
};
static QueueHandle_t radio_requests;
static esp_err_t queue_request(const radio_request &r) {
    if (xQueueSend(radio_requests, &r, 0) == pdPASS) return ESP_OK;
    // Caller still holds admission lock; undo only this request's reservation.
    return ESP_ERR_TIMEOUT;
}
extern "C" esp_err_t radio_pair_start(const uint8_t controller[4], const uint8_t candidate[4], uint32_t *id) {
    rf_pairing check = {};
    if (!rf_pair_start(&check, controller, candidate)) return ESP_ERR_INVALID_ARG;
    radio_request r = {}; r.action = PAIR_START;
    memcpy(r.controller, controller, 4); memcpy(r.candidate, candidate, 4);
    portENTER_CRITICAL(&status_lock);
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (status.ready && radio_requests && !status.registry_error && !exchange_id && !mutation_pending &&
        registry.count < CLEVERBOX_RADIATOR_CAPACITY && registry.next_id != UINT32_MAX) {
        r.id = registry.next_id; exchange_id = r.id; pair_reserved = true; exchange_pending = true;
        result = queue_request(r);
        if (result != ESP_OK) { exchange_id = 0; pair_reserved = false; exchange_pending = false; }
        else { status.pairing_duplicate = false; status.pairing_id = r.id; status.exchange_id = r.id; *id = r.id; }
    }
    portEXIT_CRITICAL(&status_lock); return result;
}
static esp_err_t pair_action(uint32_t id, radio_action action) {
    radio_request r = {}; r.action = action;
    portENTER_CRITICAL(&status_lock);
    // Legacy no-id action is allowed only for the unambiguous pending inclusion.
    if (!id && registry.count <= 1) id = status.pairing_id;
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (status.ready && radio_requests && pair_reserved && id == exchange_id &&
        (action != PAIR_SAVE || (!exchange_pending && !status.pairing_saved && status.pairing.phase == RF_PAIR_COMPLETE))) {
        r.id = id; result = queue_request(r);
    }
    portEXIT_CRITICAL(&status_lock); return result;
}
extern "C" esp_err_t radio_pair_cancel(uint32_t id) { return pair_action(id, PAIR_CANCEL); }
extern "C" esp_err_t radio_pair_save(uint32_t id) { return pair_action(id, PAIR_SAVE); }
extern "C" esp_err_t radio_control_start(uint32_t id, const rf_control_request *request) {
    if (!request) return ESP_ERR_INVALID_ARG;
    radio_request r = {}; r.action = CONTROL_START; r.control = *request;
    portENTER_CRITICAL(&status_lock);
    radiator_record *d = find_device(id);
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (d && status.ready && radio_requests && !status.registry_error && !exchange_id && !mutation_pending) {
        if (!r.control.state.size) {
            const uint8_t *bytes = d->has_control_template ? d->control_template :
                d->has_saved_template ? d->saved_template : nullptr;
            if (bytes) { r.control.state.size = CONTROLLER_STATE_BYTES; memcpy(r.control.state.bytes, bytes, CONTROLLER_STATE_BYTES); }
        }
        if (r.control.state.size) {
            rf_control check = {};
            if (!rf_control_start(&check, &r.control, d->pair.controller, d->pair.radiator, esp_timer_get_time() / 1000))
                result = ESP_ERR_INVALID_ARG;
            else {
                r.id = d->id; exchange_id = d->id; pair_reserved = false; exchange_pending = true;
                result = queue_request(r);
                if (result != ESP_OK) { exchange_id = 0; exchange_pending = false; }
                status.exchange_id = exchange_id;
            }
        }
    }
    portEXIT_CRITICAL(&status_lock); return result;
}
static esp_err_t enqueue_mutation(uint32_t id, radio_action action, const rf_frame *imported, const char *name) {
    radio_request r = {}; r.action = action;
    if (name) {
        if (!*name || strlen(name) > RADIATOR_NAME_BYTES) return ESP_ERR_INVALID_ARG;
        for (const char *c = name; *c; ++c) if ((unsigned char)*c < 32 || *c == 127) return ESP_ERR_INVALID_ARG;
        memcpy(r.name, name, strlen(name) + 1);
    }
    portENTER_CRITICAL(&status_lock);
    radiator_record *d = find_device(id);
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (d && status.ready && radio_requests && !mutation_pending &&
        (!status.registry_error || (d->state_load_error && (action == STATE_SAVE || action == STATE_CLEAR))) &&
        !(action == REMOVE && exchange_id == d->id)) {
        result = ESP_OK; r.id = d->id;
        if (action == STATE_SAVE) {
            if (imported) r.control.state = *imported;
            else if (d->has_control_template) {
                r.control.state.size = CONTROLLER_STATE_BYTES;
                memcpy(r.control.state.bytes, d->control_template, CONTROLLER_STATE_BYTES);
            } else result = ESP_ERR_INVALID_STATE;
            rf_frame checked;
            if (result == ESP_OK && (!rf_set_target(&checked, &r.control.state, RF_OFF, 0) ||
                memcmp(r.control.state.bytes + 6, d->pair.radiator, 4))) result = ESP_ERR_INVALID_ARG;
        }
        if (result == ESP_OK) {
            mutation_pending = true;
            result = queue_request(r);
            if (result != ESP_OK) mutation_pending = false;
            else {
                status.mutation_error = 0; status.mutation_pending = true;
                if (action == STATE_SAVE) d->state_save_result = RADIO_STORE_PENDING;
                if (action == STATE_CLEAR) d->state_clear_result = RADIO_STORE_PENDING;
            }
        }
    }
    portEXIT_CRITICAL(&status_lock); return result;
}
extern "C" esp_err_t radio_state_save(uint32_t id, const rf_frame *f) { return enqueue_mutation(id, STATE_SAVE, f, nullptr); }
extern "C" esp_err_t radio_state_clear(uint32_t id) { return enqueue_mutation(id, STATE_CLEAR, nullptr, nullptr); }
extern "C" esp_err_t radio_device_rename(uint32_t id, const char *name) { return name ? enqueue_mutation(id, RENAME, nullptr, name) : ESP_ERR_INVALID_ARG; }
extern "C" esp_err_t radio_device_remove(uint32_t id) { return enqueue_mutation(id, REMOVE, nullptr, nullptr); }

static int transmit_frame(const rf_frame &frame, int &rx_result)
{
    if (!rf_valid(&frame) || frame.size > 64) {
        rx_result = RADIOLIB_ERR_NONE;
        return ESP_ERR_INVALID_ARG;
    }
    // Poll, inclusion and 59-byte controller-state frames all fit in the FIFO. Fixed length sends our complete envelope
    // without RadioLib adding another length byte or a hardware CRC.
    int result = radio.standby();
    if (result == RADIOLIB_ERR_NONE) result = radio.fixedPacketLengthMode(frame.size);
    if (result == RADIOLIB_ERR_NONE) result = radio.transmit(frame.bytes, frame.size);
    // Always leave TX and restore the receive profile, including after timeout.
    rx_result = radio.standby();
    if (rx_result == RADIOLIB_ERR_NONE)
        rx_result = module.SPIsetRegValue(RADIOLIB_CC1101_REG_PKTCTRL0, 2, 1, 0);
    if (rx_result == RADIOLIB_ERR_NONE) rx_result = radio.startReceive();
    return result != RADIOLIB_ERR_NONE ? result : rx_result;
}

static void save_pair(const rf_pairing &pair, bool &saved, int &error) {
    for (size_t i = 0; i < registry.count; ++i)
        if (!memcmp(registry.records[i].pair.radiator, pair.radiator, 4)) { error = ESP_ERR_INVALID_STATE; return; }
    if (registry.count >= CLEVERBOX_RADIATOR_CAPACITY) { error = ESP_ERR_INVALID_STATE; return; }
    radiator_record d = {};
    d.id = status.pairing_id; memcpy(d.name, "Radiator", 9);
    memcpy(d.pair.controller, pair.controller, 4); memcpy(d.pair.radiator, pair.radiator, 4);
    error = device_registry_save_record(&registry, &d, 0); saved = error == ESP_OK;
    if (saved) {
        portENTER_CRITICAL(&status_lock);
        registry.records[registry.count++] = d; registry.next_id = d.id + 1;
        portEXIT_CRITICAL(&status_lock);
    }
}

static bool receive_count(uint8_t &count)
{
    // TI SWRZ020E: RXBYTES can be torn while the radio updates it.
    // Bound the consecutive-read check; try again next tick if it stays unstable.
    uint8_t previous = module.SPIreadRegister(RADIOLIB_CC1101_REG_RXBYTES | 0x40);
    for (int i = 0; i < 8; ++i) {
        count = module.SPIreadRegister(RADIOLIB_CC1101_REG_RXBYTES | 0x40);
        if (count == previous) return true;
        previous = count;
    }
    return false;
}

// One worker iteration is shared by FreeRTOS and the host's controllable clock.
struct radio_worker {
    rf_frame frame = {};
    int64_t started = 0;
    rf_pairing pair = status.pairing;
    bool saved = status.pairing_saved;
    int pair_error = status.pairing_error;
    rf_control control = {}; // Only one delivery machine/template/report snapshot globally.
    uint32_t control_id = 0;
    int control_error = 0;
    void publish() {
        portENTER_CRITICAL(&status_lock);
        status.pairing = pair; status.pairing_saved = saved; status.pairing_error = pair_error;
        radiator_record *d = find_device(control_id);
        if (control_id && d) { d->control = control.status; d->control_error = control_error; }
        if (exchange_id && !exchange_pending && (pair_reserved ? (saved || (!rf_pair_active(&pair) && pair.phase != RF_PAIR_COMPLETE)) :
                            (control_id == exchange_id && !rf_control_active(&control.status)))) {
            exchange_id = 0; pair_reserved = false;
        }
        status.exchange_id = exchange_id; status.device_count = registry.count;
        portEXIT_CRITICAL(&status_lock);
    }
    void step() {
        radio_request request;
        if (xQueueReceive(radio_requests, &request, 0) == pdPASS) {
            if (request.action >= STATE_SAVE) {
                radiator_record *d = find_device(request.id);
                int result = ESP_ERR_NOT_FOUND;
                if (d) {
                    radiator_record changed = *d;
                    if (request.action == RENAME) memcpy(changed.name, request.name, sizeof(changed.name));
                    else if (request.action != REMOVE) {
                        changed.has_saved_template = request.action == STATE_SAVE;
                        memset(changed.saved_template, 0, sizeof(changed.saved_template));
                        if (changed.has_saved_template) memcpy(changed.saved_template, request.control.state.bytes, CONTROLLER_STATE_BYTES);
                    }
                    result = device_registry_save_record(&registry, request.action == REMOVE ? nullptr : &changed,
                                                        request.action == REMOVE ? d->id : 0);
                    portENTER_CRITICAL(&status_lock);
                    if (result == ESP_OK) {
                        if (request.action == REMOVE) {
                            size_t index = d - registry.records;
                            for (size_t i = index + 1; i < registry.count; ++i) registry.records[i - 1] = registry.records[i];
                            registry.records[--registry.count] = {};
                        } else {
                            *d = changed; d->state_load_error = 0;
                            if (request.action == STATE_CLEAR) {
                                d->has_control_template = false; memset(d->control_template, 0, sizeof(d->control_template));
                                d->control_template_time_us = 0;
                            }
                        }
                    }
                    if (request.action == STATE_SAVE || request.action == STATE_CLEAR) {
                        d->state_store_error = result;
                        (request.action == STATE_SAVE ? d->state_save_result : d->state_clear_result) =
                            result == ESP_OK ? RADIO_STORE_OK : RADIO_STORE_FAILED;
                    }
                    if (result == ESP_OK) status.registry_error = 0;
                    portEXIT_CRITICAL(&status_lock);
                }
                portENTER_CRITICAL(&status_lock);
                status.mutation_error = result; mutation_pending = status.mutation_pending = false;
                portEXIT_CRITICAL(&status_lock);
            } else if (request.action == CONTROL_START) {
                radiator_record *d = find_device(request.id);
                if (d && rf_control_start(&control, &request.control, d->pair.controller, d->pair.radiator,
                                          esp_timer_get_time() / 1000)) {
                    control_id = d->id; control_error = 0;
                }
                portENTER_CRITICAL(&status_lock); exchange_pending = false; portEXIT_CRITICAL(&status_lock);
                // Reservation is deliberately still held; publish transfers it only
                // after active/terminal state is visible, including during TX.
            } else if (request.action == PAIR_CANCEL && request.id == exchange_id && pair_reserved) {
                rf_pair_cancel(&pair);
                if (pair.phase == RF_PAIR_COMPLETE && !saved) pair = {};
            } else if (request.action == PAIR_SAVE && pair.phase == RF_PAIR_COMPLETE && !saved)
                save_pair(pair, saved, pair_error);
            else if (request.action == PAIR_START) {
                saved = false; pair_error = 0; pair = {};
                portENTER_CRITICAL(&status_lock); exchange_pending = false; portEXIT_CRITICAL(&status_lock);
                rf_pair_start(&pair, request.controller, request.candidate);
            }
        }
        int64_t now_ms = esp_timer_get_time() / 1000;
        rf_pair_tick(&pair, now_ms);
        rf_control_tick(&control, now_ms);
        rf_frame outgoing = {};
        bool pairing_tx = rf_pair_next(&pair, now_ms, &outgoing);
        bool control_tx = !pairing_tx && rf_control_next(&control, now_ms, &outgoing);
        if (pairing_tx || control_tx) {
            int rx_result;
            int tx_result = transmit_frame(outgoing, rx_result);
            if (pairing_tx) {
                pair_error = tx_result;
                rf_pair_sent(&pair, esp_timer_get_time() / 1000, tx_result == 0);
            } else {
                control_error = tx_result;
                rf_control_sent(&control, esp_timer_get_time() / 1000, tx_result == 0);
            }
            frame = {}; started = 0;
            portENTER_CRITICAL(&status_lock);
            status.error = rx_result;
            status.ready = rx_result == RADIOLIB_ERR_NONE;
            portEXIT_CRITICAL(&status_lock);
        }
        if (!radio_transport_status().ready) { publish(); return; }
        uint8_t count = 0;
        bool stable = receive_count(count);
        bool restart = stable && (count & 0x80) != 0;
        bool receive_error = restart;
        int result = RADIOLIB_ERR_NONE;
        count &= 0x7f;
        if (stable && !restart && count) {
            if (!started) started = esp_timer_get_time();
            if (count > 64) {
                restart = receive_error = true;
            } else {
                // Never empty a live FIFO: TI's RX FIFO erratum can duplicate
                // the last byte when it is read as a new RF byte arrives.
                if (!frame.size && count > 1) {
                    frame.bytes[0] = module.SPIreadRegister(RADIOLIB_CC1101_REG_FIFO);
                    frame.size = 1;
                    --count;
                }
                size_t expected = (size_t)frame.bytes[0] + 3;
                if (frame.size && frame.bytes[0] >= 11) {
                    size_t remaining = expected - frame.size;
                    bool complete = count >= remaining;
                    size_t take = complete ? remaining : (count > 1 ? count - 1 : 0);
                    // Infinite mode has no hardware packet end. Stop RX before
                    // the final drain, and leave any tail/noise for the flush.
                    if (complete) result = radio.standby();
                    if (result == RADIOLIB_ERR_NONE && take) {
                        module.SPIreadRegisterBurst(RADIOLIB_CC1101_REG_FIFO | 0x40, take,
                                                   frame.bytes + frame.size);
                        frame.size += take;
                    }
                    if (result != RADIOLIB_ERR_NONE) restart = receive_error = true;
                }
                if (result == RADIOLIB_ERR_NONE && frame.size &&
                    (frame.bytes[0] < 11 || frame.size == expected)) {
                    bool valid = rf_valid(&frame);
                    rf_report report = {};
                    bool decoded = valid && rf_decode_report(&frame, &report);
                    int64_t now = esp_timer_get_time();
                    rf_pair_phase before = pair.phase;
                    if (valid) rf_pair_receive(&pair, now / 1000, &frame);
                    if (before == RF_PAIR_WAIT_REPLY && pair.phase == RF_PAIR_BIND) {
                        for (size_t i = 0; i < registry.count; ++i)
                            if (!memcmp(registry.records[i].pair.radiator, pair.radiator, 4)) {
                                pair.phase = RF_PAIR_ERROR; pair_error = ESP_ERR_INVALID_STATE;
                                portENTER_CRITICAL(&status_lock); status.pairing_duplicate = true; portEXIT_CRITICAL(&status_lock);
                            }
                    }
                    if (before != RF_PAIR_COMPLETE && pair.phase == RF_PAIR_COMPLETE)
                        save_pair(pair, saved, pair_error);
                    // Receive ownership is determined by both addresses, never UI selection.
                    rf_frame checked_state = {};
                    bool state_frame = valid && rf_set_target(&checked_state, &frame, RF_OFF, 0);
                    portENTER_CRITICAL(&status_lock);
                    for (size_t i = 0; i < registry.count; ++i) {
                        radiator_record &d = registry.records[i];
                        if (state_frame && !memcmp(frame.bytes + 1, d.pair.controller, 4) &&
                            !memcmp(frame.bytes + 6, d.pair.radiator, 4)) {
                            d.has_control_template = true;
                            memcpy(d.control_template, frame.bytes, CONTROLLER_STATE_BYTES);
                            d.control_template_time_us = now;
                        }
                        if (decoded && !memcmp(report.source, d.pair.radiator, 4) &&
                            !memcmp(report.destination, d.pair.controller, 4)) {
                            d.has_paired_report = true; d.paired_report = report; d.paired_report_time_us = now;
                            if (d.id == control_id) {
                                rf_control_receive(&control, now / 1000, &frame);
                                if (control.status.phase == RF_CONTROL_MATCHED) control_error = 0;
                            } else if ((d.control.phase == RF_CONTROL_TIMEOUT || d.control.phase == RF_CONTROL_ERROR) &&
                                       rf_control_reconcile(&d.control, now / 1000, &frame)) d.control_error = 0;
                        }
                    }
                    if (valid) {
                        ++status.valid_frames;
                        status.has_frame = true;
                        status.last_frame = frame;
                        status.frame_time_us = now;
                        if (decoded) {
                            status.has_report = true;
                            status.last_report = report;
                            status.report_time_us = now;
                        }
                    } else ++status.invalid_frames;
                    portEXIT_CRITICAL(&status_lock);
                    restart = true;
                }
            }
        }
        // A maximum 258-byte envelope takes about 56 ms at this bitrate.
        if (started && esp_timer_get_time() - started > 100000)
            restart = receive_error = true;
        if (restart) {
            if (result == RADIOLIB_ERR_NONE)
                result = radio.startReceive(); // standby, flush, re-enter RX
            portENTER_CRITICAL(&status_lock);
            if (receive_error) ++status.receive_errors;
            status.error = result;
            status.ready = result == RADIOLIB_ERR_NONE;
            portEXIT_CRITICAL(&status_lock);
            if (result != RADIOLIB_ERR_NONE) {
                ESP_LOGE("radio", "Receive failed: %d", result);
                if (rf_pair_active(&pair)) pair.phase = RF_PAIR_ERROR;
                if (rf_control_active(&control.status)) control.status.phase = RF_CONTROL_ERROR;
                pair_error = control_error = result; publish(); return;
            }
            frame = {};
            started = 0;
        }
        publish();
    }
};

static void receive_task(void *)
{
    radio_worker worker;
    for (;;) {
        worker.step();
        if (!radio_transport_status().ready) { vTaskDelete(nullptr); return; }
        vTaskDelay(1);
    }
}

extern "C" void radio_transport_start(void)
{
    status.capacity = CLEVERBOX_RADIATOR_CAPACITY;
    int load_result = device_registry_load(&registry);
    status.registry_error = load_result;
    status.device_count = registry.count;
    if (registry.count == 1) {
        status.pairing.phase = RF_PAIR_COMPLETE; status.pairing_saved = true;
        status.pairing_id = registry.records[0].id;
        memcpy(status.pairing.controller, registry.records[0].pair.controller, 4);
        memcpy(status.pairing.radiator, registry.records[0].pair.radiator, 4);
    }
    if (load_result != ESP_OK) status.pairing_error = load_result;
    radio_requests = xQueueCreate(1, sizeof(radio_request));
    // Working receive assumptions from the RF spec, not a calibrated TX profile.
    int result = radio.begin(868.3, 1000.0 / 27.0, 17.0, 101.6, 0, 32);
    status.chip_version = result == RADIOLIB_ERR_NONE ? radio.getChipVersion() : -1;
    // Preamble length is not established; do not require a 28-bit preamble.
    if (result == RADIOLIB_ERR_NONE) result = radio.setPreambleLength(32, 0);
    if (result == RADIOLIB_ERR_NONE) result = radio.setEncoding(RADIOLIB_ENCODING_NRZ);
    if (result == RADIOLIB_ERR_NONE) result = radio.setSyncWord(0xd3, 0x91);
    // CC1101 30/32-bit sync detection for the repeated D391 D391 word.
    if (result == RADIOLIB_ERR_NONE)
        result = module.SPIsetRegValue(RADIOLIB_CC1101_REG_MDMCFG2, 3, 2, 0);
    if (result == RADIOLIB_ERR_NONE) result = radio.setCrcFiltering(false);
    if (result == RADIOLIB_ERR_NONE) result = radio.disableAddressFiltering();
    // Infinite packet mode retains L and both software CRCs in the FIFO.
    // Variable-length hardware mode would stop before the outer CRC.
    if (result == RADIOLIB_ERR_NONE)
        result = module.SPIsetRegValue(RADIOLIB_CC1101_REG_PKTCTRL0, 2, 1, 0);
    if (result == RADIOLIB_ERR_NONE)
        result = module.SPIsetRegValue(RADIOLIB_CC1101_REG_PKTCTRL1, 0, 2, 2);
    if (result == RADIOLIB_ERR_NONE) result = radio.startReceive();
    // RadioLib's blocking short-frame TX completion uses GDO2 as an input.
    hal.pinMode(RADIO_GDO2, hal.GpioModeInput);
    if (!radio_requests && result == RADIOLIB_ERR_NONE) result = ESP_ERR_NO_MEM;
    status.error = result;
    status.ready = result == RADIOLIB_ERR_NONE;
    if (status.ready && xTaskCreate(receive_task, "radio_rx", 6144, nullptr, 5, nullptr) != pdPASS) {
        status.ready = false;
        status.error = ESP_ERR_NO_MEM;
        radio.standby();
    }
    ESP_LOGI("radio", "CC1101 version=%d status=%d", status.chip_version, status.error);
}
