// SPDX-License-Identifier: MIT
#pragma once
// Host stand-ins for FreeRTOS and RadioLib used by radio_transport.cpp. The
// CC1101 is a FIFO model fed by host/simulation.cpp.
#include <cstdint>
#include <cstring>
#include <vector>
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "board.h"

constexpr int pdPASS = 1;
inline int xTaskCreate(void (*)(void *), const char *, int, void *, int, void *) { return pdPASS; }
inline void vTaskDelay(int) {}

constexpr uint32_t RADIOLIB_NC = UINT32_MAX;
constexpr int RADIOLIB_ENCODING_NRZ = 0;
constexpr int RADIOLIB_CC1101_REG_PKTCTRL0 = 0x08, RADIOLIB_CC1101_REG_PKTCTRL1 = 0x07,
              RADIOLIB_CC1101_REG_MDMCFG2 = 0x12, RADIOLIB_CC1101_REG_RXBYTES = 0x3b,
              RADIOLIB_CC1101_REG_FIFO = 0x3f;

inline std::vector<uint8_t> fifo;
inline size_t fifo_used;
void simulated_transmit(const uint8_t *, size_t);
inline int injected_init, injected_receive, injected_transmit; // RadioLib-style error codes.

class EspHal {
public:
    EspHal(int, int, int) {}
    const uint32_t GpioModeInput = 0;
    void pinMode(uint32_t, uint32_t) {}
};
class Module {
public:
    Module(EspHal *, int, int, uint32_t, int) {}
    int SPIsetRegValue(int, int, int, int) { return 0; }
    uint8_t SPIreadRegister(int reg) {
        if (reg == (RADIOLIB_CC1101_REG_RXBYTES | 0x40)) return (uint8_t)(fifo.size() - fifo_used);
        return fifo_used < fifo.size() ? fifo[fifo_used++] : 0;
    }
    void SPIreadRegisterBurst(int, size_t n, uint8_t *out) {
        memcpy(out, fifo.data() + fifo_used, n); fifo_used += n;
    }
};
class CC1101 {
public:
    explicit CC1101(Module *) {}
    int begin(double, double, double, double, int, int) { return injected_init; }
    int getChipVersion() { return 0x14; }
    int setPreambleLength(int, int) { return 0; }
    int setEncoding(int) { return 0; }
    int setSyncWord(int, int) { return 0; }
    int setCrcFiltering(bool) { return 0; }
    int disableAddressFiltering() { return 0; }
    int standby() { return 0; }
    int fixedPacketLengthMode(size_t) { return 0; }
    int startReceive() { fifo.clear(); fifo_used = 0; return injected_receive; }
    int transmit(const uint8_t *p, size_t n) {
        if (injected_transmit) return injected_transmit;
        simulated_transmit(p, n); return 0;
    }
};
