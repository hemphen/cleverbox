// SPDX-License-Identifier: MIT
#pragma once
// Host-only FIFO/queue facade for the production radio worker. No RF driver runs.
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <vector>
#include "esp_err.h"
#include "esp_timer.h"
#include "board.h"
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) assert(++*(lock) == 1)
#define portEXIT_CRITICAL(lock) assert(--*(lock) == 0)
constexpr int pdPASS = 1;
inline int xTaskCreate(void (*)(void *), const char *, int, void *, int, void *) { return pdPASS; }
inline void vTaskDelete(void *) {}
inline void vTaskDelay(int) {}
using QueueHandle_t = int;
inline std::vector<uint8_t> queued;
inline size_t item_size;
inline QueueHandle_t xQueueCreate(int, size_t n) { item_size = n; return 1; }
inline int xQueueSend(QueueHandle_t, const void *p, int) {
    if (!queued.empty()) return 0;
    auto b = static_cast<const uint8_t *>(p); queued.assign(b, b + item_size); return pdPASS;
}
inline int xQueueReceive(QueueHandle_t, void *p, int) {
    if (queued.empty()) return 0;
    memcpy(p, queued.data(), item_size); queued.clear(); return pdPASS;
}
constexpr uint32_t RADIOLIB_NC = UINT32_MAX;
constexpr int RADIOLIB_ERR_NONE = 0, RADIOLIB_ENCODING_NRZ = 0;
constexpr int RADIOLIB_CC1101_REG_PKTCTRL0 = 0, RADIOLIB_CC1101_REG_RXBYTES = 1,
              RADIOLIB_CC1101_REG_FIFO = 2, RADIOLIB_CC1101_REG_MDMCFG2 = 3,
              RADIOLIB_CC1101_REG_PKTCTRL1 = 4;
inline std::vector<uint8_t> fifo;
inline size_t fifo_used;
void simulated_transmit(const uint8_t *, size_t);
class IdfHal { public: int GpioModeInput = 0; void pinMode(int, int) {} };
class Module {
public:
    Module(IdfHal *, int, int, uint32_t, int) {}
    int SPIsetRegValue(int, int, int, int) { return 0; }
    uint8_t SPIreadRegister(int reg) {
        if (reg == (RADIOLIB_CC1101_REG_RXBYTES | 0x40)) return fifo.size() - fifo_used;
        assert(reg == RADIOLIB_CC1101_REG_FIFO && fifo_used < fifo.size());
        return fifo[fifo_used++];
    }
    void SPIreadRegisterBurst(int, size_t n, uint8_t *out) {
        assert(fifo_used + n <= fifo.size()); memcpy(out, fifo.data() + fifo_used, n); fifo_used += n;
    }
};
class CC1101 {
public:
    explicit CC1101(Module *) {}
    int standby() { return 0; }
    int fixedPacketLengthMode(size_t) { return 0; }
    int transmit(const uint8_t *p, size_t n) { simulated_transmit(p, n); return 0; }
    int startReceive() { fifo.clear(); fifo_used = 0; return 0; }
    int begin(double, double, double, double, int, int) { return 0; }
    int getChipVersion() { return -1; } // No physical chip.
    int setPreambleLength(int, int) { return 0; }
    int setEncoding(int) { return 0; }
    int setSyncWord(int, int) { return 0; }
    int setCrcFiltering(bool) { return 0; }
    int disableAddressFiltering() { return 0; }
};
