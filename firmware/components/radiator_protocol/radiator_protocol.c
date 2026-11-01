// SPDX-License-Identifier: MIT
#include "radiator_protocol.h"
#include <string.h>

static uint16_t crc(const uint8_t *p, size_t n, bool reflected)
{
    uint16_t c = 0xffff;
    while (n--) {
        c ^= reflected ? *p++ : (uint16_t)*p++ << 8;
        for (int i = 0; i < 8; ++i)
            c = reflected ? (c >> 1) ^ ((c & 1) ? 0xa001 : 0)
                          : (uint16_t)(c << 1) ^ ((c & 0x8000) ? 0x8005 : 0);
    }
    return c;
}

static void seal(rf_frame *f)
{
    size_t l = f->bytes[0];
    uint16_t c = crc(f->bytes + 1, l - 2, true);
    f->bytes[l - 1] = c & 255; f->bytes[l] = c >> 8;
    c = crc(f->bytes, l + 1, false);
    f->bytes[l + 1] = c >> 8; f->bytes[l + 2] = c & 255;
}

bool rf_valid(const rf_frame *f)
{
    if (!f || f->size < 14 || f->size > RF_MAX_FRAME) return false;
    size_t l = f->bytes[0];
    if (l < 11 || f->size != l + 3 || f->bytes[5] != 0xc6) return false;
    return crc(f->bytes + 1, l - 2, true) ==
               (uint16_t)(f->bytes[l - 1] | f->bytes[l] << 8) &&
           crc(f->bytes, l + 1, false) ==
               (uint16_t)(f->bytes[l + 1] << 8 | f->bytes[l + 2]);
}

bool rf_short(rf_frame *out, const uint8_t source[4], const uint8_t dest[4], rf_command command)
{
    if (!out || !source || !dest || command < RF_OFFER || command > RF_POLL) return false;
    rf_frame f = {0};
    size_t payload = command == RF_OFFER ? 4 : 2;
    f.size = payload + 14; f.bytes[0] = payload + 11;
    memcpy(f.bytes + 1, source, 4); f.bytes[5] = 0xc6;
    memcpy(f.bytes + 6, dest, 4);
    if (command == RF_OFFER) { f.bytes[12] = 3; f.bytes[13] = 1; }
    else f.bytes[10] = command == RF_BIND ? 1 : 0x19;
    seal(&f); *out = f; return true;
}

bool rf_set_target(rf_frame *out, const rf_frame *state, rf_mode mode, int centi_c)
{
    static const uint8_t tags[] = {0x45,0x4b,0x3b,0x4c,0x8e,0xcc,0x97,0x94,0xe1,0xda,0x5b,0xb2};
    if (!out || !rf_valid(state) || state->size != 59 || mode < RF_COMFORT || mode > RF_ECO)
        return false;
    // Stage-1 application limits, matching the observed 5–30 C profile.
    if (mode != RF_OFF && (centi_c < 500 || centi_c > 3000)) return false;
    size_t pos = 10, target = 0;
    for (size_t i = 0; i < sizeof(tags); ++i) {
        if (pos >= 55 || state->bytes[pos] != tags[i]) return false;
        if (tags[i] == 0x94) target = pos + 1;
        pos += 2 + (tags[i] >> 6);
    }
    if (pos != 55 || !target) return false;
    rf_frame f = *state;
    unsigned raw = mode == RF_OFF ? 320 : (18 * centi_c + 50) / 100 + 320;
    f.bytes[target] = raw >> 8; f.bytes[target + 1] = raw & 255;
    f.bytes[target + 2] = (uint8_t)mode;
    seal(&f); *out = f; return true;
}

static int nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool rf_parse_hex(const char *hex, uint8_t *out, size_t capacity, size_t *size)
{
    if (!hex || !out || !size) return false;
    size_t n = strlen(hex);
    if (!n || n % 2 || n / 2 > capacity) return false;
    for (size_t i = 0; i < n; ++i) if (nibble(hex[i]) < 0) return false;
    for (size_t i = 0; i < n / 2; ++i) out[i] = nibble(hex[2*i]) * 16 + nibble(hex[2*i+1]);
    *size = n / 2; return true;
}

void rf_to_hex(const rf_frame *f, char out[RF_MAX_FRAME * 2 + 1])
{
    static const char digits[] = "0123456789ABCDEF";
    size_t n = f->size <= RF_MAX_FRAME ? f->size : 0;
    for (size_t i = 0; i < n; ++i) {
        out[2*i] = digits[f->bytes[i] >> 4]; out[2*i+1] = digits[f->bytes[i] & 15];
    }
    out[2*n] = 0;
}

static int temperature(uint16_t raw)
{
    int value = ((int)raw - 320) * 100;
    return (value + (value < 0 ? -9 : 9)) / 18;
}

bool rf_decode_report(const rf_frame *frame, rf_report *out)
{
    static const uint8_t tags[] = {0xdf, 0x4b, 0x8a, 0xe1, 0xda, 0x0a};
    if (!out || !rf_valid(frame) || frame->size != 38) return false;
    rf_report report = {0};
    memcpy(report.source, frame->bytes + 1, 4);
    memcpy(report.destination, frame->bytes + 6, 4);
    size_t pos = 10;
    for (size_t i = 0; i < sizeof(tags); ++i) {
        uint8_t tag = frame->bytes[pos];
        size_t size = (tag >> 6) + 1;
        if (tag != tags[i] || pos + 1 + size > 34) return false;
        const uint8_t *v = frame->bytes + pos + 1;
        if (tag == 0x4b || tag == 0x8a) {
            uint16_t raw = (uint16_t)(v[0] << 8 | v[1]);
            if (tag == 0x4b) {
                report.temperature_available = raw != 0x084c;
                if (report.temperature_available) report.temperature_centi_c = temperature(raw);
            } else {
                report.target_available = raw != 0x084c;
                if (report.target_available) report.target_centi_c = temperature(raw);
                report.mode = v[2];
            }
        }
        pos += size + 1;
    }
    if (pos != 34) return false;
    *out = report;
    return true;
}

bool rf_live_state(rf_frame *out, const rf_frame *state, const rf_frame *report,
                   const uint8_t controller[4], const uint8_t radiator[4],
                   rf_mode mode, int centi_c, unsigned clock_seconds)
{
    rf_report decoded;
    rf_frame f;
    if (!out || !controller || !radiator || clock_seconds >= 86400 ||
        !rf_set_target(&f, state, mode, centi_c) ||
        !rf_decode_report(report, &decoded) ||
        memcmp(state->bytes + 6, radiator, 4) ||
        memcmp(decoded.source, radiator, 4) || memcmp(decoded.destination, controller, 4))
        return false;
    // These offsets belong to the strictly validated report/state layouts above.
    memcpy(f.bytes + 1, controller, 4);
    memcpy(f.bytes + 6, radiator, 4);
    memcpy(f.bytes + 14, report->bytes + 16, 2); // 4B, including unavailable sentinel
    // 97 echoes the answered report's DF: byte 0, and DF byte 2 bit 0 (heating
    // now) into 97 byte 2 bit 0. 97's other bits stay from the template.
    f.bytes[31] = report->bytes[11];
    f.bytes[33] = (uint8_t)((f.bytes[33] & 0xfe) | (report->bytes[13] & 1));
    // Prototype policy: echo current E1; do not invent a thermostat/PID output.
    memcpy(f.bytes + 39, report->bytes + 23, 4);
    memset(f.bytes + 44, 0, 4); // DA is zero in the documented controller states.
    f.bytes[52] = clock_seconds / 3600;
    f.bytes[53] = clock_seconds / 60 % 60;
    f.bytes[54] = clock_seconds % 60;
    seal(&f);
    *out = f;
    return true;
}

bool rf_touch_e3_state(rf_frame *out, const rf_touch_e3_request *request,
                       const rf_report_context *context, int64_t now_ms)
{
    rf_report decoded;
    if (!out) return false;
    if (!request || !context || context->received_ms < 0 || now_ms < context->received_ms ||
        now_ms - context->received_ms >= RF_REPORT_STALE_MS ||
        request->mode < RF_COMFORT || request->mode > RF_ECO ||
        (request->mode == RF_OFF ? request->centi_c != 0 :
         (request->centi_c < 500 || request->centi_c > 3000)) ||
        request->clock_seconds >= 86400 ||
        request->boost_minutes < 1 || request->boost_minutes > 1439 ||
        (request->controller[0] != 0xf1 && request->controller[0] != 0xd0) ||
        request->controller[0] != request->radiator[0] ||
        !(request->controller[1] | request->controller[2] | request->controller[3]) ||
        !memcmp(request->controller, request->radiator, 4) ||
        !rf_decode_report(context->frame, &decoded) || context->frame->bytes[33] != 0x47 ||
        memcmp(decoded.source, request->radiator, 4) ||
        memcmp(decoded.destination, request->controller, 4)) {
        *out = (rf_frame){0};
        return false;
    }

    // Reversible Touch E3 profile assumptions, NOT synthetic-fixture defaults.
    // Observed constants and DF echo: protocol 2da47dd, backed by the captures
    // and state_fields.py in clevertouch-radio b951a717. See docs/controller-profile.md.
    // Every index includes the length byte. Unspecified value bytes are zero.
    rf_frame f = {.size = 59, .bytes = {
        [0] = 0x38, [5] = 0xc6,
        [10] = 0x45, [13] = 0x4b, [16] = 0x3b, [18] = 0x4c,
        [21] = 0x8e, [24] = 0x84, [25] = 0xcc,
        [30] = 0x97, [32] = 0x30, [33] = 0x04,
        [34] = 0x94, [38] = 0xe1, [43] = 0xda, [48] = 0x5b, [51] = 0xb2
    }};
    const rf_frame *report = context->frame;
    memcpy(f.bytes + 1, request->controller, 4);
    memcpy(f.bytes + 6, request->radiator, 4);
    memcpy(f.bytes + 14, report->bytes + 16, 2); // Exact 4B, including 084C.
    f.bytes[31] = report->bytes[11];
    // DF byte 1's local-change bit is deliberately not echoed by this profile.
    f.bytes[33] |= report->bytes[13] & 1;
    unsigned raw = request->mode == RF_OFF ? 320 : (18 * request->centi_c + 50) / 100 + 320;
    f.bytes[35] = raw >> 8; f.bytes[36] = raw & 255; f.bytes[37] = request->mode;
    memcpy(f.bytes + 39, report->bytes + 23, 4); // Exact E1; DA remains zero.
    f.bytes[49] = request->boost_minutes >> 8; f.bytes[50] = request->boost_minutes & 255;
    f.bytes[52] = request->clock_seconds / 3600;
    f.bytes[53] = request->clock_seconds / 60 % 60;
    f.bytes[54] = request->clock_seconds % 60;
    seal(&f);
    *out = f;
    return true;
}
