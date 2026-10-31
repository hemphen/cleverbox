// SPDX-License-Identifier: MIT
#pragma once
#include <RadioLib.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "board.h"
#include <cstring>

// Small ESP-IDF HAL using public drivers; RadioLib's example HAL is ESP32-only.
class IdfHal : public RadioLibHal {
public:
    IdfHal() : RadioLibHal(0, 1, 0, 1, GPIO_INTR_POSEDGE, GPIO_INTR_NEGEDGE) {}
    // Errors are sticky for one owner operation. Suppress subsequent SPI after
    // failure/deadline so a partial failed TX cannot issue a later TX strobe.
    void begin_operation(unsigned ms) {
        failure = ESP_OK; deadline_us = ms ? esp_timer_get_time() + ms * 1000LL : 0;
        if (pending) finish_pending();
    }
    int error() { check_deadline(); return failure; }
    void record_error(int e) { if (!failure && e) failure = e; }
    void yield() override { vTaskDelay(1); check_deadline(); }
    void init() override { spiBegin(); }
    void term() override { spiEnd(); }
    void pinMode(uint32_t pin, uint32_t mode) override {
        if (pin == RADIOLIB_NC) return;
        gpio_set_direction((gpio_num_t)pin, mode ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT);
    }
    void digitalWrite(uint32_t pin, uint32_t value) override {
        if (pin != RADIOLIB_NC) gpio_set_level((gpio_num_t)pin, value);
    }
    uint32_t digitalRead(uint32_t pin) override {
        return pin == RADIOLIB_NC ? 0 : gpio_get_level((gpio_num_t)pin);
    }
    // Receive task polls the FIFO; this stage does not use RadioLib interrupts.
    void attachInterrupt(uint32_t, void (*)(void), uint32_t) override {}
    void detachInterrupt(uint32_t) override {}
    void delay(RadioLibTime_t ms) override {
        if (ms) vTaskDelay(pdMS_TO_TICKS(ms) + 1);
        check_deadline();
    }
    void delayMicroseconds(RadioLibTime_t us) override { esp_rom_delay_us(us); }
    RadioLibTime_t millis() override { return esp_timer_get_time() / 1000; }
    RadioLibTime_t micros() override { return esp_timer_get_time(); }
    long pulseIn(uint32_t, uint32_t, RadioLibTime_t) override { return 0; }
    void spiBegin() override {
        if (error() || bus_acquired) return;
        if (!bus_owned) {
            spi_bus_config_t bus = {};
            bus.mosi_io_num = RADIO_MOSI; bus.miso_io_num = RADIO_MISO;
            bus.sclk_io_num = RADIO_SCK; bus.quadwp_io_num = -1; bus.quadhd_io_num = -1;
            int e = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_DISABLED);
            record_error(e); if (e) return;
            bus_owned = true;
        }
        spi_device_interface_config_t device = {};
        device.clock_speed_hz = 2000000; device.spics_io_num = -1; device.queue_size = 1;
        if (!spi) record_error(spi_bus_add_device(SPI2_HOST, &device, &spi));
        if (!error()) {
            // IDF 5.5.1 rejects finite acquire/start timeouts. This dedicated
            // bus has one device, one owner and no queued/interrupt transfers.
            // Acquire once; start's background wait has nothing to wait for.
            int e = spi_device_acquire_bus(spi, portMAX_DELAY);
            record_error(e); bus_acquired = e == ESP_OK;
        }
    }
    void spiBeginTransaction() override {}
    void spiTransfer(uint8_t *out, size_t len, uint8_t *in) override {
        if (in) memset(in, 0, len); // No uninitialised/fabricated bytes on failure.
        if (error()) return;
        if (!spi) { record_error(ESP_ERR_INVALID_STATE); return; }
        for (size_t offset = 0; offset < len && !error(); offset += 64) {
            size_t chunk = len - offset < 64 ? len - offset : 64;
            memset(tx, 0, sizeof(tx)); memset(rx, 0, sizeof(rx));
            if (out) memcpy(tx, out + offset, chunk);
            transaction = {}; transaction.length = chunk * 8;
            transaction.tx_buffer = tx; transaction.rx_buffer = rx;
            int e = spi_device_polling_start(spi, &transaction, portMAX_DELAY);
            record_error(e); if (e) return;
            pending = true;
            if (!finish_pending()) return;
            if (in) memcpy(in + offset, rx, chunk);
        }
    }
    void spiEndTransaction() override {}
    // Static appliance resources live for the worker's lifetime. RadioLib calls
    // term() when detection fails; do not tear down a possibly in-flight bus.
    void spiEnd() override {}
private:
    spi_device_handle_t spi = nullptr;
    bool bus_owned = false, bus_acquired = false, pending = false;
    int failure = ESP_OK;
    int64_t deadline_us = 0;
    // A timed-out IDF transaction may still be in flight. Its descriptor and
    // buffers must outlive the call. Never reuse them or release the bus until
    // polling_end confirms completion, including on later explicit retries.
    static_assert(sizeof(spi_transaction_t) <= 64, "Review SPI transaction storage bound");
    spi_transaction_t transaction = {};
    alignas(4) uint8_t tx[64] = {}, rx[64] = {};
    bool finish_pending() {
        int e = spi_device_polling_end(spi, pdMS_TO_TICKS(20) + 1);
        record_error(e);
        if (e) return false;
        pending = false; return true;
    }
    void check_deadline() {
        if (deadline_us && esp_timer_get_time() >= deadline_us) record_error(ESP_ERR_TIMEOUT);
    }
};
