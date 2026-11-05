// SPDX-License-Identifier: MIT
// Real RadioLib CC1101 driver against a register model, not RF simulation. This
// HAL supplies deterministic registers, strobe responses, a GDO pulse and a clock.
#include <RadioLib.h>
#include <array>
#include <cassert>
#include <cstdio>
#include <vector>

static_assert(RADIOLIB_VERSION == 0x07080100, "Review expectations when testing another release");
class RegisterHal : public RadioLibHal {
public:
    RegisterHal() : RadioLibHal(0, 1, 0, 1, 1, 2) { reset(); }
    std::array<uint8_t, 64> regs{};
    std::vector<uint8_t> fifo, sent;
    unsigned long us = 0, fstxon_at = 0, tx_at = 0;
    bool calibrating = false, transmitting = false, no_start = false, no_end = false;
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
    // Time passes while the library yields during waits.
    void yield() override { us += 1000; }
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
                case 0x36: regs[0x35] = 1; calibrating = transmitting = false; break;
                case 0x35:
                    assert((regs[8] & 7) == 0); // fixed TX, hardware CRC off
                    assert(regs[6] == fifo.size());
                    assert((regs[0] & 0x3f) == 6); // GDO2 sync sent / packet end
                    regs[0x3a] = fifo.size();
                    sent = fifo; tx_at = us; transmitting = true; calibrating = false; regs[0x35] = 0x13; break;
                case 0x3b: fifo.clear(); break;
            }
            return;
        }
        for (size_t i = 1; i < len; ++i) {
            unsigned reg = addr == 0x3f ? addr : (addr + i - 1);
            if (out[0] & 0x80) {
                if (reg == 0x35 && calibrating && us - fstxon_at >= 200) regs[reg] = 0x12;
                if (transmitting && !no_end && us - tx_at >= 2000) { regs[0x35]=1; regs[0x3a]=0; }
                in[i] = regs[reg];
            } else if (reg == 0x3f) fifo.push_back(out[i]);
            else regs[reg] = out[i];
        }
    }
};

// The production setup sequence and arguments (radio_transport.cpp initialize()).
static void configure(CC1101& radio, Module& module) {
    assert(radio.begin(868.3, 1000.0 / 27.0, 17.0, 101.6, 0, 32) == 0);
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
// The production transmit sequence (radio_transport.cpp transmit()).
static int transmit(CC1101& radio, Module& module, const uint8_t* bytes, size_t length) {
    assert(radio.standby() == 0);
    assert(radio.fixedPacketLengthMode(length) == 0);
    int result = radio.transmit(bytes, length);
    assert(module.SPIsetRegValue(0x08, 2, 1, 0) == 0);
    assert(radio.startReceive() == 0);
    return result;
}
int main() {
    RegisterHal hal;
    Module module(&hal, 18, 15, RADIOLIB_NC, 14); // CS18, GDO0=15, GDO2=14 as in board.h.
    CC1101 radio(&module);
    configure(radio, module);
    // Golden RF profile; see docs/hardware.md.
    const uint8_t expected[][2] = {
        {0x03,0x0f},{0x04,0xd3},{0x05,0x91},{0x07,0},{0x08,2},
        {0x0d,0x21},{0x0e,0x65},{0x0f,0x6a},{0x10,0xca},{0x11,0x75},
        {0x12,3},{0x13,0x20},{0x14,0},{0x15,0x32},{0x17,0},{0x18,0x10},{0x3e,0x50}
    };
    for (const auto& setting : expected) assert(hal.regs[setting[0]] == setting[1]);
    const auto profile = hal.regs;
    // Fixed-length frames go out exactly as given: no length, address or CRC added.
    const uint8_t packet[] = {0x0b,0xf1,1,2,3,0xc6,0xf1,4,5,6,0,0,0,0};
    for (size_t length : {sizeof(packet), size_t(59), size_t(64)}) {
        std::vector<uint8_t> bytes(length);
        for (size_t i = 0; i < length; ++i) bytes[i] = packet[i % sizeof(packet)];
        assert(transmit(radio, module, bytes.data(), bytes.size()) == 0);
        assert(hal.sent == bytes && hal.gdo_reads > 0);
        // Back in infinite-length receive with the same profile.
        assert((hal.regs[0x08] & 7) == 2 && hal.regs[0x35] == 0x0d);
        for (unsigned reg = 3; reg <= 0x2e; ++reg)
            if (reg != 6) assert(hal.regs[reg] == profile[reg]); // PKTLEN keeps the TX length.
    }
    // A GDO2 that never rises or never falls ends in a bounded TX timeout.
    for (bool start_failure : {true, false}) {
        hal.no_start = start_failure; hal.no_end = !start_failure;
        unsigned long before = hal.us;
        assert(transmit(radio, module, packet, sizeof(packet)) == RADIOLIB_ERR_TX_TIMEOUT);
        assert(hal.us - before < 100000);
        hal.no_start = hal.no_end = false;
        assert((hal.regs[0x08] & 7) == 2 && hal.regs[0x35] == 0x0d);
    }
    std::printf("RadioLib %d.%d.%d: RF profile, fixed-length TX, TX timeouts and RX restore passed\n",
                RADIOLIB_VERSION_MAJOR, RADIOLIB_VERSION_MINOR, RADIOLIB_VERSION_PATCH);
}
