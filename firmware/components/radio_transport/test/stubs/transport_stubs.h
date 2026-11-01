// SPDX-License-Identifier: MIT
#pragma once
#include <cassert>
#include <mutex>
#include <cstddef>
#include <cstdint>
#include <cstring>

using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_ERR_NOT_FOUND = 1, ESP_ERR_INVALID_STATE = 2,
              ESP_ERR_INVALID_ARG = 3, ESP_ERR_TIMEOUT = 4, ESP_ERR_NO_MEM = 5;
#define ESP_ERROR_CHECK(call) assert((call) == ESP_OK)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)

using SemaphoreHandle_t = std::mutex *;
inline constexpr int portMAX_DELAY = -1;
inline SemaphoreHandle_t xSemaphoreCreateMutex() { static std::mutex m; return &m; }
inline int xSemaphoreTake(SemaphoreHandle_t m, int) { m->lock(); return 1; }
inline int xSemaphoreGive(SemaphoreHandle_t m) { m->unlock(); return 1; }
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
inline uint8_t queued_bytes[65536];
inline size_t queue_item_size, queue_capacity, queue_count, queue_head;
inline bool queue_full;
inline void (*dequeue_hook)() = nullptr;
inline QueueHandle_t xQueueCreate(int capacity, size_t size) {
    assert((size_t)capacity * size <= sizeof(queued_bytes)); queue_item_size = size;
    queue_capacity = capacity; queue_count = queue_head = 0; return 1;
}
inline int xQueueSend(QueueHandle_t, const void *item, int) {
    if (queue_full || queue_count == queue_capacity) return 0;
    memcpy(queued_bytes + ((queue_head + queue_count) % queue_capacity) * queue_item_size, item, queue_item_size);
    ++queue_count; return pdPASS;
}
inline unsigned uxQueueMessagesWaiting(QueueHandle_t) { return queue_count; }
inline int xQueueReceive(QueueHandle_t, void *item, int) {
    if (!queue_count) return 0;
    memcpy(item, queued_bytes + queue_head * queue_item_size, queue_item_size);
    queue_head = (queue_head + 1) % queue_capacity; --queue_count;
    if (dequeue_hook) dequeue_hook();
    return pdPASS;
}

using RadioLibTime_t = uint64_t;
constexpr uint32_t RADIOLIB_NC = UINT32_MAX;
constexpr int RADIOLIB_ERR_TX_TIMEOUT = -5;
constexpr int RADIOLIB_CC1101_CMD_TX = 0x35;
constexpr int RADIOLIB_ERR_NONE = 0, RADIOLIB_ENCODING_NRZ = 0;
constexpr int RADIOLIB_CC1101_REG_PKTCTRL0 = 0, RADIOLIB_CC1101_REG_RXBYTES = 1,
              RADIOLIB_CC1101_REG_FIFO = 2, RADIOLIB_CC1101_REG_MDMCFG2 = 3,
              RADIOLIB_CC1101_REG_PKTCTRL1 = 4, RADIOLIB_CC1101_REG_TXBYTES = 5, RADIOLIB_CC1101_REG_MARCSTATE = 6, RADIOLIB_CC1101_REG_IOCFG2 = 7;
constexpr int RADIOLIB_CC1101_MARC_STATE_IDLE = 1, RADIOLIB_CC1101_MARC_STATE_TXFIFO_UNDERFLOW = 0x16;
constexpr int RADIOLIB_CC1101_GDOX_HIGH_Z = 0x2e;
inline void (*transmit_hook)() = nullptr;
inline int transmit_result;
inline int receive_result, begin_result, standby_result;
inline unsigned begin_count;
inline bool tx_strobe = true, tx_gpio_high;
inline uint8_t tx_fifo_count = 0, tx_marc = 1;
inline unsigned finish_count;
inline int finish_result;
inline void (*poll_hook)() = nullptr;
inline unsigned tx_status_reads;
inline int gdo_mapping = 0x2e;
inline uint8_t transmitted_bytes[64];
inline size_t transmitted_size;
inline unsigned transmit_count;
inline uint8_t incoming_bytes[64];
inline size_t incoming_size, incoming_used;
inline unsigned count_reads, receive_starts;
inline bool receiving = true;
inline int packet_mode = 2;
inline size_t fixed_length;
inline uint8_t (*count_hook)(uint8_t) = nullptr;
class RadioLibHal {
public:
    RadioLibHal(int, int, int, int, int, int) {}
    virtual ~RadioLibHal() = default;
    int GpioModeInput = 0;
    virtual void yield() {}
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
inline RadioLibHal *test_hal;
class Module {
public:
    Module(RadioLibHal *h, int, int, uint32_t, int) { test_hal=h; }
    int SPIsetRegValue(int reg, int value, int, int) {
        if (reg == RADIOLIB_CC1101_REG_IOCFG2) gdo_mapping = value;
        if (reg == RADIOLIB_CC1101_REG_PKTCTRL0) packet_mode = value;
        return 0;
    }
    uint8_t SPIreadRegister(int reg) {
        if (reg == (RADIOLIB_CC1101_REG_TXBYTES | 0x40)) {
            ++tx_status_reads;
            if (poll_hook) poll_hook();
            return tx_fifo_count;
        }
        if (reg == (RADIOLIB_CC1101_REG_MARCSTATE | 0x40)) return tx_marc;
        if (reg == (RADIOLIB_CC1101_REG_RXBYTES | 0x40)) {
            ++count_reads;
            uint8_t count = incoming_size - incoming_used;
            return count_hook ? count_hook(count) : count;
        }
        assert(reg == RADIOLIB_CC1101_REG_FIFO && incoming_used < incoming_size);
        assert(!receiving || incoming_size - incoming_used > 1);
        return incoming_bytes[incoming_used++];
    }
    void SPIreadRegisterBurst(int, size_t size, uint8_t *out) {
        assert(incoming_used + size <= incoming_size);
        assert(!receiving || incoming_used + size < incoming_size);
        memcpy(out, incoming_bytes + incoming_used, size); incoming_used += size;
    }
};
class CC1101 {
public:
    explicit CC1101(Module *) {}
    int standby() { receiving = false; return standby_result; }
    int fixedPacketLengthMode(size_t size) { packet_mode = 0; fixed_length = size; return 0; }
    int startTransmit(const uint8_t *bytes, size_t size) {
        if (tx_strobe) { uint8_t stx = 0x35, in; test_hal->spiBegin(); test_hal->spiTransfer(&stx, 1, &in); }
        gdo_mapping = 6;
        assert(size <= sizeof(transmitted_bytes) && packet_mode == 0 && size == fixed_length);
        memcpy(transmitted_bytes, bytes, size); transmitted_size = size;
        ++transmit_count;
        if (transmit_hook) transmit_hook();
        return transmit_result;
    }
    int finishTransmit() { ++finish_count; if (!finish_result) gdo_mapping = 0x2e; return finish_result; }
    int startReceive() {
        ++receive_starts; incoming_size = incoming_used = 0;
        receiving = receive_result == 0; return receive_result;
    }
    int begin(double, double, double, double, int, int) { ++begin_count; return begin_result; }
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
inline uint32_t gpio_get_level(int pin) { assert(pin == 14); return tx_gpio_high; }
inline void esp_rom_delay_us(RadioLibTime_t) {}
inline int spi_bus_initialize(int, const spi_bus_config_t *, int) { return 0; }
inline int spi_bus_add_device(int, const spi_device_interface_config_t *, void **p) { *p=(void *)1; return 0; }
inline int spi_acquire_result, spi_start_result, spi_end_result;
inline unsigned spi_starts;
inline int spi_device_acquire_bus(void *, int wait) { assert(wait==portMAX_DELAY); return spi_acquire_result; }
inline void spi_device_release_bus(void *) {}
inline int spi_device_polling_start(void *, spi_transaction_t *, int wait) { assert(wait==portMAX_DELAY); ++spi_starts; return spi_start_result; }
inline int spi_device_polling_end(void *, int) { return spi_end_result; }
inline void spi_bus_remove_device(void *) {}
inline void spi_bus_free(int) {}

inline uint32_t esp_random() { static uint32_t n = 1000; return ++n; }
