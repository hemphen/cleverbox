// SPDX-License-Identifier: MIT
#pragma once
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_ERR_NOT_FOUND = 1, ESP_ERR_INVALID_STATE = 2,
              ESP_ERR_INVALID_ARG = 3, ESP_ERR_TIMEOUT = 4, ESP_ERR_NO_MEM = 5;
#define ESP_ERROR_CHECK(call) assert((call) == ESP_OK)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)

using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) assert(++*(lock) == 1)
#define portEXIT_CRITICAL(lock) assert(--*(lock) == 0)
#define pdMS_TO_TICKS(ms) (ms)
constexpr int pdPASS = 1;
inline int64_t test_now_us = 1000000;
inline int64_t esp_timer_get_time() { return test_now_us; }
inline void (*delay_hook)() = nullptr;
inline void vTaskDelay(int) { if (delay_hook) delay_hook(); }
inline void vTaskDelete(void *) { assert(false); }
inline int xTaskCreate(void (*)(void *), const char *, int, void *, int, void *) { return pdPASS; }

using QueueHandle_t = int;
inline uint8_t queued_bytes[1024];
inline size_t queue_item_size;
inline bool queue_full;
inline void (*dequeue_hook)() = nullptr;
inline QueueHandle_t xQueueCreate(int, size_t size) {
    assert(size <= sizeof(queued_bytes)); queue_item_size = size; return 1;
}
inline int xQueueSend(QueueHandle_t, const void *item, int) {
    if (queue_full) return 0;
    memcpy(queued_bytes, item, queue_item_size); queue_full = true; return pdPASS;
}
inline int xQueueReceive(QueueHandle_t, void *item, int) {
    if (!queue_full) return 0;
    memcpy(item, queued_bytes, queue_item_size); queue_full = false;
    if (dequeue_hook) dequeue_hook();
    return pdPASS;
}

using RadioLibTime_t = uint64_t;
constexpr uint32_t RADIOLIB_NC = UINT32_MAX;
constexpr int RADIOLIB_ERR_NONE = 0, RADIOLIB_ENCODING_NRZ = 0;
constexpr int RADIOLIB_CC1101_REG_PKTCTRL0 = 0, RADIOLIB_CC1101_REG_RXBYTES = 1,
              RADIOLIB_CC1101_REG_FIFO = 2, RADIOLIB_CC1101_REG_MDMCFG2 = 3,
              RADIOLIB_CC1101_REG_PKTCTRL1 = 4;
inline void (*transmit_hook)() = nullptr;
inline int transmit_result;
inline uint8_t incoming_bytes[64];
inline size_t incoming_size, incoming_used;
class RadioLibHal {
public:
    RadioLibHal(int, int, int, int, int, int) {}
    virtual ~RadioLibHal() = default;
    int GpioModeInput = 0;
    virtual void init() = 0;
    virtual void term() = 0;
    virtual void pinMode(uint32_t, uint32_t) = 0;
    virtual void digitalWrite(uint32_t, uint32_t) = 0;
    virtual uint32_t digitalRead(uint32_t) = 0;
    virtual void attachInterrupt(uint32_t, void (*)(void), uint32_t) = 0;
    virtual void detachInterrupt(uint32_t) = 0;
    virtual void delay(RadioLibTime_t) = 0;
    virtual void delayMicroseconds(RadioLibTime_t) = 0;
    virtual RadioLibTime_t millis() = 0;
    virtual RadioLibTime_t micros() = 0;
    virtual long pulseIn(uint32_t, uint32_t, RadioLibTime_t) = 0;
    virtual void spiBegin() = 0;
    virtual void spiBeginTransaction() = 0;
    virtual void spiTransfer(uint8_t *, size_t, uint8_t *) = 0;
    virtual void spiEndTransaction() = 0;
    virtual void spiEnd() = 0;
};
class Module {
public:
    Module(RadioLibHal *, int, int, uint32_t, int) {}
    int SPIsetRegValue(int, int, int, int) { return 0; }
    uint8_t SPIreadRegister(int reg) {
        if (reg == (RADIOLIB_CC1101_REG_RXBYTES | 0x40)) return incoming_size - incoming_used;
        assert(reg == RADIOLIB_CC1101_REG_FIFO && incoming_used < incoming_size);
        return incoming_bytes[incoming_used++];
    }
    void SPIreadRegisterBurst(int, size_t size, uint8_t *out) {
        assert(incoming_used + size <= incoming_size);
        memcpy(out, incoming_bytes + incoming_used, size); incoming_used += size;
    }
};
class CC1101 {
public:
    explicit CC1101(Module *) {}
    int standby() { return 0; }
    int fixedPacketLengthMode(size_t) { return 0; }
    int transmit(const uint8_t *, size_t) {
        if (transmit_hook) transmit_hook();
        return transmit_result;
    }
    int startReceive() { return 0; }
    int begin(double, double, double, double, int, int) { return 0; }
    int getChipVersion() { return 1; }
    int setPreambleLength(int, int) { return 0; }
    int setEncoding(int) { return 0; }
    int setSyncWord(int, int) { return 0; }
    int setCrcFiltering(bool) { return 0; }
    int disableAddressFiltering() { return 0; }
};

using gpio_num_t = int;
using spi_device_handle_t = void *;
constexpr int GPIO_INTR_POSEDGE = 0, GPIO_INTR_NEGEDGE = 1,
              GPIO_MODE_OUTPUT = 1, GPIO_MODE_INPUT = 0, SPI2_HOST = 0, SPI_DMA_DISABLED = 0;
struct spi_bus_config_t { int mosi_io_num, miso_io_num, sclk_io_num, quadwp_io_num, quadhd_io_num; };
struct spi_device_interface_config_t { int clock_speed_hz, spics_io_num, queue_size; };
struct spi_transaction_t { size_t length; const void *tx_buffer; void *rx_buffer; };
inline void gpio_set_direction(int, int) {}
inline void gpio_set_level(int, uint32_t) {}
inline uint32_t gpio_get_level(int) { return 0; }
inline void esp_rom_delay_us(RadioLibTime_t) {}
inline int spi_bus_initialize(int, const spi_bus_config_t *, int) { return 0; }
inline int spi_bus_add_device(int, const spi_device_interface_config_t *, void **) { return 0; }
inline int spi_device_polling_transmit(void *, spi_transaction_t *) { return 0; }
inline void spi_bus_remove_device(void *) {}
inline void spi_bus_free(int) {}
