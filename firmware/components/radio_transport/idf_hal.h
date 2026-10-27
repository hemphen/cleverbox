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

// Small ESP-IDF HAL using public drivers; RadioLib's example HAL is ESP32-only.
class IdfHal : public RadioLibHal {
public:
    IdfHal() : RadioLibHal(0, 1, 0, 1, GPIO_INTR_POSEDGE, GPIO_INTR_NEGEDGE) {}
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
    }
    void delayMicroseconds(RadioLibTime_t us) override { esp_rom_delay_us(us); }
    RadioLibTime_t millis() override { return esp_timer_get_time() / 1000; }
    RadioLibTime_t micros() override { return esp_timer_get_time(); }
    long pulseIn(uint32_t, uint32_t, RadioLibTime_t) override { return 0; }
    void spiBegin() override {
        spi_bus_config_t bus = {};
        bus.mosi_io_num = RADIO_MOSI; bus.miso_io_num = RADIO_MISO;
        bus.sclk_io_num = RADIO_SCK; bus.quadwp_io_num = -1; bus.quadhd_io_num = -1;
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_DISABLED));
        spi_device_interface_config_t device = {};
        device.clock_speed_hz = 2000000; device.spics_io_num = -1;
        device.queue_size = 1;
        ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &device, &spi));
    }
    void spiBeginTransaction() override {}
    void spiTransfer(uint8_t *out, size_t len, uint8_t *in) override {
        // Non-DMA SPI transactions are limited to the 64-byte hardware FIFO.
        // CS stays asserted by RadioLib across these chunks.
        for (size_t offset = 0; offset < len; offset += 64) {
            size_t chunk = len - offset < 64 ? len - offset : 64;
            spi_transaction_t t = {};
            t.length = chunk * 8;
            t.tx_buffer = out ? out + offset : nullptr;
            t.rx_buffer = in ? in + offset : nullptr;
            ESP_ERROR_CHECK(spi_device_polling_transmit(spi, &t));
        }
    }
    void spiEndTransaction() override {}
    void spiEnd() override {
        spi_bus_remove_device(spi); spi_bus_free(SPI2_HOST);
    }
private:
    spi_device_handle_t spi = nullptr;
};
