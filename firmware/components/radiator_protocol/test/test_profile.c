// SPDX-License-Identifier: MIT
#include "radiator_protocol.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

struct profile_vector {
    rf_touch_e3_request request;
    const char *report, *expected;
};
#include "profile_vectors.inc"

static rf_frame parse(const char *hex)
{
    rf_frame frame = {0};
    assert(rf_parse_hex(hex, frame.bytes, sizeof(frame.bytes), &frame.size));
    return frame;
}

static void matches(const rf_frame *actual, const char *hex)
{
    rf_frame expected = parse(hex);
    assert(rf_valid(actual) && rf_valid(&expected));
    assert(actual->size == 59 && actual->size == expected.size);
    assert(!memcmp(actual->bytes, expected.bytes, expected.size));
}

static void rejected(const rf_touch_e3_request *request,
                     const rf_report_context *context, int64_t now_ms)
{
    // A failure must invalidate even an already usable output buffer.
    rf_frame out = parse(vectors[0].expected), empty = {0};
    assert(!rf_touch_e3_state(&out, request, context, now_ms));
    assert(!out.size && !memcmp(out.bytes, empty.bytes, sizeof(out.bytes)));
    assert(!rf_valid(&out));
}

static void complete_vectors(void)
{
    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
        const struct profile_vector *v = &vectors[i];
        rf_frame report = parse(v->report), out;
        rf_report_context context = {.frame = &report, .received_ms = 1000};
        rf_frame original = report;
        rf_touch_e3_request request = v->request;
        assert(rf_touch_e3_state(&out, &request, &context, 1000));
        matches(&out, v->expected); // All 59 bytes, including both independent CRCs.
        assert(!memcmp(&request, &v->request, sizeof(request)));
        assert(!memcmp(&report, &original, sizeof(report)));
        assert(!memcmp(out.bytes + 14, report.bytes + 16, 2));
        assert(!memcmp(out.bytes + 39, report.bytes + 23, 4));
        for (unsigned b = 44; b <= 47; ++b) assert(!out.bytes[b]);
        // The function is pure and does not advance time or remember a report.
        assert(rf_touch_e3_state(&out, &request, &context, 1001));
        matches(&out, v->expected);
        // Staging also permits output to alias the report input on success.
        assert(rf_touch_e3_state(&report, &request, &context, 1000));
        matches(&report, v->expected);
    }
}

static void inputs_and_freshness(void)
{
    rf_touch_e3_request request = vectors[0].request;
    rf_frame report = parse(vectors[0].report), out;
    rf_report_context context = {.frame = &report, .received_ms = 0};
    assert(rf_touch_e3_state(&out, &request, &context, 0));
    assert(rf_touch_e3_state(&out, &request, &context, RF_REPORT_STALE_MS - 1));
    matches(&out, vectors[0].expected);
    rejected(&request, &context, RF_REPORT_STALE_MS);
    rejected(&request, &context, INT64_MAX);
    rejected(&request, &context, -1);
    context.received_ms = -1; rejected(&request, &context, 0);
    context.received_ms = INT64_MIN; rejected(&request, &context, INT64_MAX);
    context.received_ms = 1001; rejected(&request, &context, 1000);
    context.received_ms = INT64_MAX;
    assert(rf_touch_e3_state(&out, &request, &context, INT64_MAX));
    context.received_ms = INT64_MAX - RF_REPORT_STALE_MS + 1;
    assert(rf_touch_e3_state(&out, &request, &context, INT64_MAX));
    --context.received_ms; rejected(&request, &context, INT64_MAX);
    context.received_ms = 0;
    assert(!rf_touch_e3_state(NULL, &request, &context, 0));
    rejected(NULL, &context, 0);
    rejected(&request, NULL, 0);
    context.frame = NULL; rejected(&request, &context, 0);
    context.frame = &report;

    const int targets[] = {INT_MIN, -1, 0, 499, 3001, INT_MAX};
    for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); ++i) {
        request.centi_c = targets[i]; rejected(&request, &context, 0);
    }
    request = vectors[0].request;
    request.mode = RF_OFF;
    rejected(&request, &context, 0); // Off input is explicitly 0 C, not a hidden target.
    request.centi_c = 0;
    assert(rf_touch_e3_state(&out, &request, &context, 0));
    request = vectors[0].request;
    const int modes[] = {-1, 4, 8, 11, 99, INT_MAX};
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
        request.mode = (rf_mode)modes[i]; rejected(&request, &context, 0);
    }
    request = vectors[0].request;
    request.clock_seconds = 86400; rejected(&request, &context, 0);
    request.clock_seconds = UINT_MAX; rejected(&request, &context, 0);
    request = vectors[0].request;
    request.boost_minutes = 0; rejected(&request, &context, 0);
    request.boost_minutes = 1440; rejected(&request, &context, 0);
    request.boost_minutes = UINT_MAX; rejected(&request, &context, 0);
}

static void report_validity_and_routing(void)
{
    rf_touch_e3_request request = vectors[0].request;
    rf_frame report = parse(vectors[0].report), original = report;
    rf_report_context context = {.frame = &report, .received_ms = 0};
    // Unknown/reordered layout and unknown 0A value, even with valid CRCs.
    const char *invalid[] = {bad_tail, bad_tag, bad_inner, bad_outer};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        report = parse(invalid[i]);
        if (i < 2) assert(rf_valid(&report));
        else assert(!rf_valid(&report));
        rejected(&request, &context, 0);
    }
    // Invalid identities with matching CRC-valid report routing must still fail.
    for (size_t i = 0; i < sizeof(invalid_pairs) / sizeof(invalid_pairs[0]); ++i) {
        size_t size;
        request = vectors[0].request;
        assert(rf_parse_hex(invalid_pairs[i].controller, request.controller, 4, &size) && size == 4);
        assert(rf_parse_hex(invalid_pairs[i].radiator, request.radiator, 4, &size) && size == 4);
        report = parse(invalid_pairs[i].report); assert(rf_valid(&report));
        rejected(&request, &context, 0);
    }
    request = vectors[0].request;
    // Every wire byte participates in validation, including length and both CRCs.
    for (size_t b = 0; b < original.size; ++b) {
        report = original; report.bytes[b] ^= 1;
        rejected(&request, &context, 0);
    }
    report = original;
    for (size_t n = 0; n < original.size; ++n) {
        report.size = n; rejected(&request, &context, 0);
    }
    report.size = original.size + 1; rejected(&request, &context, 0);
    report.size = RF_MAX_FRAME + 1; rejected(&request, &context, 0);
    report = parse(vectors[0].expected); rejected(&request, &context, 0);
    report = original;
    // Wrong controller, wrong radiator, crossed/reversed relationships.
    request.controller[3] ^= 1; rejected(&request, &context, 0);
    request = vectors[0].request;
    request.radiator[3] ^= 1; rejected(&request, &context, 0);
    request = vectors[0].request;
    memcpy(request.controller, vectors[0].request.radiator, 4);
    memcpy(request.radiator, vectors[0].request.controller, 4);
    rejected(&request, &context, 0);
    // Independently CRC-valid reports for other address pairs cannot be reused.
    request = vectors[0].request;
    report = parse(vectors[4].report); rejected(&request, &context, 0);
    report = parse(vectors[9].report); rejected(&request, &context, 0);
    report = original;
    request.radiator[0] = 0xd0; rejected(&request, &context, 0);
    request = vectors[0].request;
    request.controller[0] = request.radiator[0] = 0xa1;
    rejected(&request, &context, 0);
    request = vectors[0].request;
    memset(request.controller + 1, 0, 3); rejected(&request, &context, 0);
    request = vectors[0].request;
    memcpy(request.radiator, request.controller, 4); rejected(&request, &context, 0);
}

int main(void)
{
    complete_vectors();
    inputs_and_freshness();
    report_validity_and_routing();
    puts("Explicit Touch E3 profile full bytes, context, bounds and routing: passed");
}
