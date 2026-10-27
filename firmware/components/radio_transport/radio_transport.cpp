// SPDX-License-Identifier: MIT
#include "idf_hal.h"
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

enum radio_action { PAIR_START, PAIR_CANCEL, PAIR_SAVE, CONTROL_START };
struct radio_request { radio_action action; uint8_t controller[4], candidate[4]; rf_control_request control; };
static QueueHandle_t radio_requests;
static bool store_load_failed;

static esp_err_t enqueue_pair(const radio_request &request)
{
    radio_status current = radio_transport_status();
    if (!current.ready || !radio_requests) return ESP_ERR_INVALID_STATE;
    if (request.action == PAIR_START) {
        rf_pairing check = current.pairing;
        if (current.pairing_saved || store_load_failed ||
            !rf_pair_start(&check, request.controller, request.candidate))
            return ESP_ERR_INVALID_STATE;
    }
    if (request.action == PAIR_SAVE &&
        (current.pairing_saved || current.pairing.phase != RF_PAIR_COMPLETE))
        return ESP_ERR_INVALID_STATE;
    return xQueueSend(radio_requests, &request, 0) == pdPASS ? ESP_OK : ESP_ERR_TIMEOUT;
}

extern "C" esp_err_t radio_pair_start(const uint8_t controller[4], const uint8_t candidate[4])
{
    radio_request request = {};
    request.action = PAIR_START;
    memcpy(request.controller, controller, 4);
    memcpy(request.candidate, candidate, 4);
    return enqueue_pair(request);
}
extern "C" esp_err_t radio_pair_cancel(void)
{
    radio_request request = {}; request.action = PAIR_CANCEL;
    return enqueue_pair(request);
}
extern "C" esp_err_t radio_pair_save(void)
{
    radio_request request = {}; request.action = PAIR_SAVE;
    return enqueue_pair(request);
}

extern "C" esp_err_t radio_control_start(const rf_control_request *request)
{
    radio_status current = radio_transport_status();
    if (!current.ready || !current.pairing_saved || !radio_requests ||
        rf_control_active(&current.control)) return ESP_ERR_INVALID_STATE;
    rf_control check = {};
    if (!request || !rf_control_start(&check, request, current.pairing.controller,
                                     current.pairing.radiator, esp_timer_get_time() / 1000))
        return ESP_ERR_INVALID_ARG;
    radio_request queued = {}; queued.action = CONTROL_START; queued.control = *request;
    return xQueueSend(radio_requests, &queued, 0) == pdPASS ? ESP_OK : ESP_ERR_TIMEOUT;
}

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

static void save_pair(const rf_pairing &pair, bool &saved, int &error)
{
    paired_device device = {};
    memcpy(device.controller, pair.controller, 4);
    memcpy(device.radiator, pair.radiator, 4);
    error = device_store_save(&device);
    saved = error == ESP_OK;
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

static void receive_task(void *)
{
    rf_frame frame = {};
    int64_t started = 0;
    rf_pairing pair = status.pairing;
    bool saved = status.pairing_saved;
    int pair_error = status.pairing_error;
    rf_control control = {};
    int control_error = 0;
    for (;;) {
        radio_request request;
        if (xQueueReceive(radio_requests, &request, 0) == pdPASS) {
            if (request.action == CONTROL_START && saved) {
                if (rf_control_start(&control, &request.control, pair.controller, pair.radiator,
                                     esp_timer_get_time() / 1000)) control_error = 0;
            }
            else if (request.action == PAIR_CANCEL) rf_pair_cancel(&pair);
            else if (request.action == PAIR_SAVE && pair.phase == RF_PAIR_COMPLETE && !saved)
                save_pair(pair, saved, pair_error);
            else if (request.action == PAIR_START && !saved &&
                     pair.phase != RF_PAIR_COMPLETE) {
                // A prior timeout/TX error can be retried explicitly.
                pair_error = 0;
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
        if (!radio_transport_status().ready) {
            portENTER_CRITICAL(&status_lock);
            status.pairing = pair; status.pairing_saved = saved; status.pairing_error = pair_error;
            status.control = control.status; status.control_error = control_error;
            portEXIT_CRITICAL(&status_lock);
            vTaskDelete(nullptr); return;
        }
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
                    if (before != RF_PAIR_COMPLETE && pair.phase == RF_PAIR_COMPLETE)
                        save_pair(pair, saved, pair_error);
                    bool paired_report = saved && decoded &&
                        !memcmp(report.source, pair.radiator, 4) &&
                        !memcmp(report.destination, pair.controller, 4);
                    if (paired_report) {
                        rf_control_receive(&control, now / 1000, &frame);
                        if (control.status.phase == RF_CONTROL_MATCHED) control_error = 0;
                    }
                    portENTER_CRITICAL(&status_lock);
                    if (paired_report) {
                        status.has_paired_report = true;
                        status.paired_report = report;
                        status.paired_report_time_us = now;
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
                portENTER_CRITICAL(&status_lock);
                status.pairing = pair; status.pairing_saved = saved; status.pairing_error = result;
                status.control = control.status; status.control_error = result;
                portEXIT_CRITICAL(&status_lock);
                vTaskDelete(nullptr);
                return;
            }
            frame = {};
            started = 0;
        }
        portENTER_CRITICAL(&status_lock);
        status.pairing = pair; status.pairing_saved = saved; status.pairing_error = pair_error;
        status.control = control.status; status.control_error = control_error;
        portEXIT_CRITICAL(&status_lock);
        vTaskDelay(1);
    }
}

extern "C" void radio_transport_start(void)
{
    paired_device device = {};
    int load_result = device_store_load(&device);
    if (load_result == ESP_OK) {
        status.pairing.phase = RF_PAIR_COMPLETE;
        memcpy(status.pairing.controller, device.controller, 4);
        memcpy(status.pairing.radiator, device.radiator, 4);
        status.pairing_saved = true;
    } else if (load_result != ESP_ERR_NOT_FOUND) {
        status.pairing_error = load_result; store_load_failed = true;
    }
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
