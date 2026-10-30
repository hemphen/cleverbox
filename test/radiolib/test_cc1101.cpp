// SPDX-License-Identifier: MIT
// Compatibility probes of REAL RadioLib, not RF simulation. This HAL supplies
// deterministic registers, strobe responses, a GDO pulse and a monotonic clock.
#include <RadioLib.h>
#include <array>
#include <cassert>
#include <cstdio>
#include <vector>

static_assert(RADIOLIB_VERSION == 0x07030000 || RADIOLIB_VERSION == 0x07080100,
              "Review expectations when testing another release");
constexpr bool candidate = RADIOLIB_VERSION >= 0x07080100;
class RegisterHal : public RadioLibHal {
public:
    RegisterHal() : RadioLibHal(0, 1, 0, 1, 1, 2) { reset(); }
    std::array<uint8_t, 64> regs{};
    std::vector<uint8_t> fifo, sent;
    unsigned long us = 0, fstxon_at = 0, tx_at = 0;
    unsigned calibration_us = 200;
    int refuse_register = -1;
    bool calibrating = false, transmitting = false, no_start = false, no_end = false;
    bool idle_failure = false;
    unsigned gdo_reads = 0;
    void reset() {
        regs.fill(0); regs[0x31] = 0x14; regs[0x35] = 1;
        calibrating = transmitting = false;
    }
    void pinMode(uint32_t, uint32_t) override {}
    void digitalWrite(uint32_t, uint32_t) override {}
    uint32_t digitalRead(uint32_t pin) override {
        // Same Module mapping as production: CS18, IRQ/GDO0=15, GPIO/GDO2=14.
        assert(pin == 14); ++gdo_reads; us += 100;
        if (!transmitting || no_start) return 0;
        return no_end || us - tx_at < 2000;
    }
    void attachInterrupt(uint32_t, void (*)(void), uint32_t) override { assert(false); }
    void detachInterrupt(uint32_t) override {}
    void delay(RadioLibTime_t ms) override { us += ms * 1000; }
    void delayMicroseconds(RadioLibTime_t n) override { us += n; }
    RadioLibTime_t millis() override { us += 100; return us / 1000; }
    RadioLibTime_t micros() override { us += 10; return us; }
    long pulseIn(uint32_t, uint32_t, RadioLibTime_t) override { return 0; }
    // Deliberately inherit the empty base yield, as the production IdfHal does.
    void spiBegin() override {}
    void spiEnd() override {}
    void spiBeginTransaction() override {}
    void spiEndTransaction() override {}
    void spiTransfer(uint8_t* out, size_t len, uint8_t* in) override {
        assert(len); us += 10;
        for (size_t i = 0; i < len; ++i) in[i] = 0;
        uint8_t addr = out[0] & 0x3f;
        if (len == 1) {
            switch (addr) {
                case 0x30: reset(); break;
                case 0x31: fstxon_at = us; calibrating = true; regs[0x35] = 8; break;
                case 0x34: regs[0x35] = 0x0d; break;
                case 0x36:
                    if (!idle_failure) { regs[0x35] = 1; calibrating = transmitting = false; }
                    break;
                case 0x35:
                    assert((regs[8] & 7) == 0); // fixed TX, hardware CRC off
                    assert(regs[6] == fifo.size());
                    assert((regs[0] & 0x3f) == 6); // GDO2 sync sent / packet end
                    sent = fifo; tx_at = us; transmitting = true; calibrating = false; regs[0x35] = 0x13; break;
                case 0x3b: fifo.clear(); break;
            }
            return;
        }
        for (size_t i = 1; i < len; ++i) {
            unsigned reg = addr == 0x3f ? addr : (addr + i - 1);
            if (out[0] & 0x80) {
                if (reg == 0x35 && calibrating && us - fstxon_at >= calibration_us) regs[reg] = 0x12;
                if (reg == 0x35 && transmitting && !no_end && us - tx_at >= 2000) regs[reg] = 1;
                in[i] = regs[reg];
            } else if (reg == 0x3f) fifo.push_back(out[i]);
            else if (static_cast<int>(reg) != refuse_register) regs[reg] = out[i];
        }
    }
};

// The production sequence and arguments; both versions must retain this profile.
static void configure(CC1101& radio, Module& module, float bandwidth) {
    assert(radio.begin(868.3, 1000.0 / 27.0, 17.0, bandwidth, 0, 32) == 0);
    assert(radio.setPreambleLength(32, 0) == 0);
    assert(radio.setEncoding(RADIOLIB_ENCODING_NRZ) == 0);
    assert(radio.setSyncWord(0xd3, 0x91) == 0);
    assert(module.SPIsetRegValue(0x12, 3, 2, 0) == 0);
    assert(radio.setCrcFiltering(false) == 0);
    assert(radio.disableAddressFiltering() == 0);
    assert(module.SPIsetRegValue(0x08, 2, 1, 0) == 0);
    assert(module.SPIsetRegValue(0x07, 0, 2, 2) == 0);
    assert(radio.startReceive() == 0);
}
static void restore(CC1101& radio, Module& module, RegisterHal& hal) {
    assert(radio.standby() == 0);
    assert(module.SPIsetRegValue(0x08, 2, 1, 0) == 0);
    assert(radio.startReceive() == 0);
    assert((hal.regs[0x08] & 7) == 2); // infinite RX, CRC off
    assert((hal.regs[0x02] & 0x7f) == 1); // GDO0 RX FIFO full/end
    assert(hal.regs[0x35] == 0x0d);
}
int main() {
    RegisterHal hal;
    Module module(&hal, 18, 15, RADIOLIB_NC, 14);
    CC1101 radio(&module);
    configure(radio, module, 101.6);
    assert((hal.regs[0x10] & 0xf0) == 0xc0);
    // The corrected range comparison affects unsupported values, not 101.6.
    assert(radio.setRxBandwidth(60.0) == (candidate ? RADIOLIB_ERR_INVALID_RX_BANDWIDTH : 0));
    assert(radio.setRxBandwidth(101.6) == 0);
    assert(radio.startReceive() == 0);
    // Golden stable profile from the baseline; excludes status/calibration and
    // IOCFG2, whose deliberate post-TX high-Z change is checked separately.
    const uint8_t expected[][2] = {
        {0x03,0x0f},{0x04,0xd3},{0x05,0x91},{0x07,0},{0x08,2},
        {0x0d,0x21},{0x0e,0x65},{0x0f,0x6a},{0x10,0xca},{0x11,0x75},
        {0x12,3},{0x13,0x20},{0x14,0},{0x15,0x32},{0x17,0},{0x18,0x10},{0x3e,0x50}
    };
    std::printf("profile:");
    for (const auto& setting : expected) {
        assert(hal.regs[setting[0]] == setting[1]);
        std::printf(" %02x=%02x", setting[0], hal.regs[setting[0]]);
    }
    std::puts("");
    const auto profile = hal.regs;
    auto transmit = [&](const uint8_t* bytes, size_t length) {
        assert(radio.standby() == 0);
        assert(radio.fixedPacketLengthMode(length) == 0);
        return radio.transmit(bytes, length);
    };
    const uint8_t packet[] = {0x0b,0xf1,1,2,3,0xc6,0xf1,4,5,6,0,0,0,0};
    for (size_t length : {sizeof(packet), size_t(59), size_t(64)}) {
        std::vector<uint8_t> bytes(length);
        for (size_t i=0; i<length; ++i) bytes[i] = packet[i % sizeof(packet)];
        assert(transmit(bytes.data(), bytes.size()) == 0);
        assert(hal.sent == bytes); // no library length/address/CRC added
        assert(hal.gdo_reads > 0);
        assert((hal.regs[0] & 0x3f) == (candidate ? 0x2e : 6));
        restore(radio, module, hal);
        assert(hal.regs[0x3e] == profile[0x3e]);
        for (unsigned reg=3; reg<=0x2e; ++reg)
            if (reg != 6) assert(hal.regs[reg] == profile[reg]); // PKTLEN retains TX length
    }
    // Slow calibration is accepted by the new 1600 us guard, old 800 us fails.
    hal.calibration_us = 1100;
    assert(transmit(packet, sizeof(packet)) == (candidate ? 0 : RADIOLIB_ERR_TX_TIMEOUT));
    restore(radio, module, hal);
    hal.calibration_us = 100000;
    assert(transmit(packet, sizeof(packet)) == RADIOLIB_ERR_TX_TIMEOUT);
    restore(radio, module, hal);
    hal.calibration_us = 200;
    for (bool start_failure : {true, false}) {
        hal.no_start = start_failure; hal.no_end = !start_failure;
        unsigned long before = hal.us;
        assert(transmit(packet, sizeof(packet)) == RADIOLIB_ERR_TX_TIMEOUT);
        assert(hal.us - before < 100000); // synthetic clock, not a physical bound
        hal.no_start = hal.no_end = false;
        restore(radio, module, hal);
    }
    // New cleanup writes can fail; the error must reach transmit's caller.
    hal.regs[0] = 6; hal.refuse_register = 0;
    assert(transmit(packet, sizeof(packet)) == (candidate ? RADIOLIB_ERR_SPI_WRITE_FAILED : 0));
    hal.refuse_register = -1;
    restore(radio, module, hal);
    // Real Module readback error must propagate from RX setup.
    hal.regs[2] = 0x2e; hal.refuse_register = 2;
    assert(radio.startReceive() == RADIOLIB_ERR_SPI_WRITE_FAILED);
    hal.refuse_register = -1;
    restore(radio, module, hal);
    hal.idle_failure = true;
    assert(radio.standby() == RADIOLIB_ERR_UNKNOWN);
    assert(radio.startReceive() == RADIOLIB_ERR_UNKNOWN);
    std::printf("RadioLib %d.%d.%d: profile, fixed TX, GDO2, state-entry/completion timeouts, RX restore and register failures passed\n",
                RADIOLIB_VERSION_MAJOR, RADIOLIB_VERSION_MINOR, RADIOLIB_VERSION_PATCH);
}
