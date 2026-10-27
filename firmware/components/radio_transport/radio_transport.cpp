// SPDX-License-Identifier: MIT
#include "idf_hal.h"
#include "radio_transport.h"
#include "esp_log.h"

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

static void receive_task(void *)
{
    rf_frame frame = {};
    int64_t started = 0;
    for (;;) {
        // CC1101 status registers need the burst-read bit even for one byte.
        uint8_t count = module.SPIreadRegister(RADIOLIB_CC1101_REG_RXBYTES | 0x40);
        bool restart = (count & 0x80) != 0;
        bool receive_error = restart;
        count &= 0x7f;
        if (!restart && count) {
            if (!frame.size) started = esp_timer_get_time();
            if (count > 64 || frame.size + count > sizeof(frame.bytes)) {
                restart = receive_error = true;
            } else {
                module.SPIreadRegisterBurst(RADIOLIB_CC1101_REG_FIFO | 0x40, count,
                                           frame.bytes + frame.size);
                frame.size += count;
                size_t expected = (size_t)frame.bytes[0] + 3;
                if (frame.bytes[0] < 11 || frame.size >= expected) {
                    // Bytes beyond the envelope are receiver tail/noise.
                    if (frame.size >= expected) frame.size = expected;
                    bool valid = rf_valid(&frame);
                    rf_report report = {};
                    bool decoded = valid && rf_decode_report(&frame, &report);
                    int64_t now = esp_timer_get_time();
                    portENTER_CRITICAL(&status_lock);
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
        if (frame.size && esp_timer_get_time() - started > 100000)
            restart = receive_error = true;
        if (restart) {
            int result = radio.startReceive(); // standby, flush, re-enter RX
            portENTER_CRITICAL(&status_lock);
            if (receive_error) ++status.receive_errors;
            status.error = result;
            status.ready = result == RADIOLIB_ERR_NONE;
            portEXIT_CRITICAL(&status_lock);
            if (result != RADIOLIB_ERR_NONE) {
                ESP_LOGE("radio", "Receive restart failed: %d", result);
                vTaskDelete(nullptr);
                return;
            }
            frame = {};
        }
        vTaskDelay(1);
    }
}

extern "C" void radio_transport_start(void)
{
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
    status.error = result;
    status.ready = result == RADIOLIB_ERR_NONE;
    if (status.ready && xTaskCreate(receive_task, "radio_rx", 4096, nullptr, 5, nullptr) != pdPASS) {
        status.ready = false;
        status.error = ESP_ERR_NO_MEM;
        radio.standby();
    }
    ESP_LOGI("radio", "CC1101 version=%d status=%d", status.chip_version, status.error);
}
