// SPDX-License-Identifier: MIT
// Compile the same worker/admission code as the transport tests, with host I/O.
#include "../firmware/components/radio_transport/radio_transport.cpp"
#include "simulation.h"
#include <algorithm>
#include <deque>
#include <string>

static int64_t now_us = 1000000;
extern "C" int64_t esp_timer_get_time(void) { return now_us; }
static radio_worker *worker;
static std::string scenario = "normal";
struct delivery { int64_t due; rf_frame frame; };
static std::deque<delivery> deliveries;
static std::deque<std::string> transmissions; // Bounded diagnostics, synthetic bytes only.
static bool offered, bound;
static uint16_t target = 698;
static uint8_t mode;
static const uint8_t controller[] = {0xf1,1,2,3}, radiator[] = {0xf1,4,5,6};
static const char *fixture = "38F1010203C6F10405064500004B02A83B004C00008E000084CC00000000972030049402BA00E100641919DA000000005B0078B20C0000A20F60D9";
static rf_frame parse(const char *hex) {
    rf_frame f = {}; bool ok = rf_parse_hex(hex, f.bytes, sizeof(f.bytes), &f.size);
    assert(ok); return f;
}
static void schedule(const rf_frame &f, int delay = 0) {
    if (deliveries.size() >= 16) return;
    int ms = delay ? delay : scenario == "delayed" ? 2500 : 300;
    deliveries.push_back({now_us + ms * 1000LL, f});
    std::stable_sort(deliveries.begin(), deliveries.end(), [](const delivery &a, const delivery &b) { return a.due < b.due; });
}
// Independent simulator envelope writer; deliberately never calls rf_short,
// rf_set_target or rf_live_state. Literal exchanges below are independent vectors.
static uint16_t checksum(const uint8_t *p, size_t n, bool reflected) {
    uint16_t value = 0xffff;
    for (size_t i = 0; i < n * 8; ++i) {
        unsigned bit = reflected ? (p[i / 8] >> (i % 8)) & 1 : (p[i / 8] >> (7 - i % 8)) & 1;
        unsigned feedback = bit ^ (reflected ? value & 1 : value >> 15);
        value = reflected ? value >> 1 : value << 1;
        if (feedback) value ^= reflected ? 0xa001 : 0x8005;
    }
    return value;
}
static void seal(rf_frame &f) {
    size_t l = f.bytes[0]; uint16_t c = checksum(f.bytes + 1, l - 2, true);
    f.bytes[l - 1] = c; f.bytes[l] = c >> 8;
    c = checksum(f.bytes, l + 1, false); f.bytes[l + 1] = c >> 8; f.bytes[l + 2] = c;
}
static rf_frame report() {
    rf_frame f = parse("23F1040506C6F1010203DF203000404B02A88A02BA00E100641919DA000A000A0A47CD48C59D");
    f.bytes[19] = target >> 8; f.bytes[20] = target; f.bytes[21] = mode; seal(f); return f;
}
void simulated_transmit(const uint8_t *p, size_t n) {
    rf_frame f = {}; assert(n <= sizeof(f.bytes)); memcpy(f.bytes, p, n); f.size = n;
    char hex[RF_MAX_FRAME * 2 + 1]; rf_to_hex(&f, hex);
    if (transmissions.size() == 16) transmissions.pop_front();
    transmissions.push_back(hex);
    if (scenario == "silence" || !rf_valid(&f)) return;
    // One invented controller/candidate/radiator only. Whole short-frame vectors
    // enforce payload AND addresses, instead of trusting the HTTP request.
    if (!strcmp(hex, "0FF1010203C6F1100001000003011FF3EA1F")) {
        offered = true; schedule(parse("0DF1040506C6F1010203000080E1D2A1"));
    } else if (offered && !strcmp(hex, "0DF1010203C6F10405060100AA1B1F72")) {
        bound = true; schedule(parse("0FF1040506C6F101020301000A47E6E64D5A"));
    } else if (bound && !strcmp(hex, "0DF1010203C6F10405061900A01B4377")) {
        schedule(report()); // ACK-only still provides the initial report needed for TX.
    } else if (bound && n == 59 && !memcmp(p + 1, controller, 4) && !memcmp(p + 6, radiator, 4)) {
        static const size_t offsets[] = {10,13,16,18,21,25,30,34,38,43,48,51};
        static const uint8_t tags[] = {0x45,0x4b,0x3b,0x4c,0x8e,0xcc,0x97,0x94,0xe1,0xda,0x5b,0xb2};
        for (size_t i = 0; i < sizeof(tags); ++i) if (p[offsets[i]] != tags[i]) return;
        unsigned raw = p[35] * 256 + p[36];
        if (p[37] > 3 || (p[37] == 1 ? raw != 320 : raw < 410 || raw > 860) ||
            p[52] > 23 || p[53] > 59 || p[54] > 59) return;
        // Assume application of a structurally valid command, except ACK-only.
        schedule(parse("0FF1040506C6F101020301000A47E6E64D5A"), 100);
        if (scenario != "ack_only") {
            target = raw; mode = p[37]; schedule(report());
        }
    }
}
extern "C" void simulation_init(void) {
    radio_transport_start(); worker = new radio_worker;
    // Saved pairing belongs to the application; a simulated radiator remembers
    // its bind across process restarts. No boot report or automatic TX.
    bound = status.pairing_saved && !memcmp(status.pairing.controller, controller, 4) &&
            !memcmp(status.pairing.radiator, radiator, 4);
}
extern "C" void simulation_advance(int ms) {
    // One millisecond worker ticks, regardless of how the test/interactive clock advances.
    for (int i = 0; i < ms; ++i) {
        now_us += 1000;
        if (fifo.empty() && !deliveries.empty() && deliveries.front().due <= now_us) {
            const rf_frame &f = deliveries.front().frame;
            fifo.assign(f.bytes, f.bytes + f.size); fifo_used = 0; deliveries.pop_front();
        }
        worker->step();
    }
}
extern "C" int simulation_scenario(const char *name) {
    if (!name || (strcmp(name,"normal") && strcmp(name,"delayed") && strcmp(name,"silence") && strcmp(name,"ack_only"))) return 0;
    scenario = name; deliveries.clear(); return 1;
}
extern "C" int simulation_inject(const char *hex) {
    rf_frame f = {};
    if (!rf_parse_hex(hex, f.bytes, 64, &f.size) || f.size < 14 || f.bytes[0] + 3U != f.size) return 0;
    schedule(f, 1); return 1; // Do not validate here: normal receive worker must reject corruption.
}
extern "C" void simulation_fixture(void) { schedule(parse(fixture), 1); }
extern "C" void simulation_status(cJSON *j) {
    cJSON_AddBoolToObject(j, "simulation", true);
    cJSON_AddStringToObject(j, "scenario", scenario.c_str());
    cJSON_AddNumberToObject(j, "simulation_time_ms", now_us / 1000);
    cJSON *tx = cJSON_AddArrayToObject(j, "simulation_tx");
    for (const auto &s : transmissions) cJSON_AddItemToArray(tx, cJSON_CreateString(s.c_str()));
}
