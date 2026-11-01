# Explicit controller profile

`rf_touch_e3_state` builds one Cleverbox replacement-controller profile from
explicit inputs and a fresh radiator report. It needs no controller-state
capture or per-radiator template. Production `rf_control_next` uses this builder as its sole normal state-frame
path. See [delivery and context policy](architecture.md#command-delivery) and
[legacy compatibility](architecture.md#templates).
The builder creates bytes, never RF work. It adds no transmission trigger.

## Contract

The header `radiator_protocol.h` defines `rf_touch_e3_request` and
`rf_report_context`. The request supplies both four-byte identities, target,
mode, local seconds since midnight and Boost minutes. There are no defaults.

- Identities are distinct and share family `F1` or `D0`. The controller's low
  three bytes must not all be zero. This follows the existing pairing guards;
  accepting the inclusion family's `D0` for this profile is a reversible product
  assumption, not evidence of ordinary `D0` controller-state traffic.
- Modes are Comfort (0), Off (1), Frost (2) and Eco (3). Non-Off targets are
  500–3000 centidegrees Celsius, inclusive. Encoding is
  `320 + floor((18 * centi_c + 50) / 100)`, matching the existing codec's rounding.
  Off requires input 0 and sends raw 320. This stricter input contract does not
  change the legacy builder's Off normalization.
- Local clock is 0–86399 seconds, inclusive. The caller supplies a usable local
  time; this codec neither obtains time nor validates SNTP synchronization.
- Boost minutes are explicitly 1–1439, inclusive, encoded as a big-endian u16.
  This is a conservative product limit within the observed range, not a proven
  wire limit. Zero behavior is unknown and is rejected. No Boost mode, countdown
  or timer is implemented. Encoding this field does not start Boost.
- Context supplies a complete report frame and its original receipt timestamp.
  Both CRCs, exact 38-byte `DF 4B 8A E1 DA 0A` layout and `0A=47` must validate.
  Its source must equal the requested radiator and its destination the requested
  controller. Unavailable temperatures and unfamiliar report modes, DF bits and
  duty bytes are retained as raw context; they do not authorize extra modes.
- Receipt and `now_ms` use the same current-boot monotonic millisecond clock.
  Require `0 <= received_ms <= now_ms` and age **strictly below 300000 ms**,
  the existing `RF_REPORT_STALE_MS` observation threshold. Exactly five minutes
  is stale. Zero is a valid receipt time, not a missing-context sentinel; a null
  frame means missing context. Ordered nonnegative subtraction avoids overflow.
  The codec never reads a wall clock, caches context or refreshes its timestamp.

The caller must retain the actual receipt time and current-boot provenance.
The age check alone is not proof of a report answering a particular poll. It
cannot replace the production command machine's fresh-report routing, admission,
deadline or readback rules. The command machine enforces those rules before using this builder.

Failure returns false and clears a non-null output, including its size, so a
previous usable frame cannot survive a failed call. Success writes a staged
59-byte frame. Inputs are unchanged unless output aliases the report frame;
that in-place use is supported. No allocation or platform dependency is added.

## Normal command duration

Production supplies **120 minutes** (`5B=0078`) for Comfort, Off, Frost and Eco.
This is an explicit wire-profile choice, not a default borrowed from a synthetic
fixture, a timer, a countdown or an autonomous expiry claim. No Boost mode or
scheduled follow-up command is added. The builder itself still requires an
explicit duration input and retains its existing range contract.

The pinned [Boost evidence](https://github.com/hemphen/clevertouch-radio/blob/b951a71721a28989298f884b1fc075a90e569cb8/docs/evidence/radiator-rf.md#boost-time-5b)
records 120 as the ordinary default, a re-pairing reset from 116 to 120, and a
return from 117 to 120 after cancelling Boost. The observed configured 1439 and
remainders 116/117 do not conflict with selecting this one non-Boost profile.
They are not per-radiator capabilities. Saved template `5B` values are ignored.
Physical behavior under this profile still requires acceptance testing.

## Complete wire layout

Offsets include the length byte. All value words below are big-endian unless
stated otherwise. Record widths follow `(tag >> 6) + 1`; no padding or terminator
is present. Payload is exactly 45 bytes at offsets 10–54.

| Offset | Bytes | Content |
| --- | ---: | --- |
| 0 | 1 | Length `38` (56 bytes excluding length and outer CRC) |
| 1–4 | 4 | Caller controller identity |
| 5 | 1 | Marker `C6` |
| 6–9 | 4 | Caller radiator identity |
| 10–12 | 3 | `45 0000` |
| 13–15 | 3 | `4B`, then exact report bytes 16–17, including `084C` unavailable |
| 16–17 | 2 | `3B 00` |
| 18–20 | 3 | `4C 0000` |
| 21–24 | 4 | `8E 000084` |
| 25–29 | 5 | `CC 00000000` |
| 30–33 | 4 | `97`, report DF byte 0, `30`, `04 \| (DF byte 2 & 01)` |
| 34–37 | 4 | `94`, encoded target u16, mode u8 |
| 38–42 | 5 | `E1`, then exact report bytes 23–26; no clamping or duty calculation |
| 43–47 | 5 | `DA 00000000` |
| 48–50 | 3 | `5B`, explicit Boost minutes u16 |
| 51–54 | 4 | `B2`, binary hour/minute/second derived from local seconds |
| 55–56 | 2 | CRC-16/MODBUS over bytes 1–54, **little-endian** |
| 57–58 | 2 | CRC-16/CMS over bytes 0–56, **big-endian** |

Both CRCs start at `FFFF`, use polynomial `8005` (reflected `A001` for MODBUS)
and xor-out zero. The outer CRC includes the completed inner CRC. Radio preamble,
sync and receiver trailing bits are outside this frame.

## Evidence and reversible assumptions

The protocol reference is
[clevertouch-protocol `2da47dde`](https://github.com/hemphen/clevertouch-protocol/blob/2da47ddeee2e8dc5193c7fc9f0ec8d76b943ef61/docs/radiator-rf/specification.md),
sections Records, State words and Constructing controller_state. That revision
was still current when this profile was implemented. Its reconstruction rule
uses an earlier captured state; selecting fixed observed constants here is an
explicit Cleverbox product policy, not a claim that the specification mandates
universal defaults.

Underlying evidence is in private clevertouch-radio revision
[`b951a717`](https://github.com/hemphen/clevertouch-radio/tree/b951a71721a28989298f884b1fc075a90e569cb8):

| Profile choice | Captured evidence and limits |
| --- | --- |
| `45=0000`, `3B=00`, `4C=0000`, `8E=000084`, `CC=00000000` | [Constant records](https://github.com/hemphen/clevertouch-radio/blob/b951a71721a28989298f884b1fc075a90e569cb8/docs/evidence/radiator-rf.md#constant-records): one value each in 433 distinct states, covering seven modes, Boost, 55% power limit, rating change, re-pairing and heating on/off |
| `97=[DF0,30,04\|heating]` | [State word and echo](https://github.com/hemphen/clevertouch-radio/blob/b951a71721a28989298f884b1fc075a90e569cb8/docs/evidence/radiator-rf.md#state-word-and-echo-df-97): 128/128 answered reports. No captured state answers a report with the local-change bit set; forcing byte 1 to `30` then remains a profile assumption |
| Exact `4B`/`E1`, zero `DA` | [Reconstruction](https://github.com/hemphen/clevertouch-radio/blob/b951a71721a28989298f884b1fc075a90e569cb8/docs/evidence/radiator-rf.md#constructing-controller_state): 119/119 states rebuilt from a prior state plus report; `DA` zero in 502/502 controller frames |
| Explicit `5B` input | [Boost time](https://github.com/hemphen/clevertouch-radio/blob/b951a71721a28989298f884b1fc075a90e569cb8/docs/evidence/radiator-rf.md#boost-time-5b): controller owns minutes; observed 116, 117, 120 and 1439, not one per-radiator capability |

`analysis/state_fields.py` and `analysis/test_state_fields.py` pin those findings
to the committed captures. The original passive capture
`captures/rf/radiator_captures.jsonl` was also inspected directly: an independent
polynomial-division CRC check validated all 41 complete state frames with
all five constant records and zero DA identical. Receiver bits after the full
frame were excluded. This cross-check agrees with the source decoder's inventory. No household identities or captured frames are copied here.

Unknown record semantics, fault/unavailable-temperature behavior, the local-change
echo case, new controller identities and ordinary `D0` acceptance remain unproven.
The profile chooses one reversible behavior; captures are evidence, not controller
personalities or capability descriptions. There is no thermostat or PID inference.

## Independent vectors and validation

`firmware/components/radiator_protocol/test/profile_vectors.inc` commits complete
report and expected output bytes with invented identities. The first pair equals
the public protocol's synthetic example, anchoring envelope and CRC conventions.
The other payloads are manually written record sequences in `profile_vectors.py`;
they are not emitted by this builder, `rf_live_state`, or another product builder.
Raw targets used include 410 (5 C), 446 (7 C), 599 (15.5 C), 689 (20.51 C rounded),
690 (20.53 C rounded), 698 (21 C), 860 (30 C), and 320 (Off).

The independent Python oracle seals those manual payloads using polynomial long
division of an augmented bit string. It reverses input bytes and the result for
MODBUS and checks `123456789` against MODBUS `4B37` and CMS `AEE7`. The C codec uses
a byte-at-a-time shift register instead. `--check` verifies the committed vector
file without rewriting it; `--write` is an explicit maintainer operation. The C
test compares all 59 output bytes, including both checksums, against that file.

`make test` checks the oracle and C profile tests, including all four modes,
multiple F1/D0 identity pairs, complete echoes, unavailable and arbitrary raw
values, known DF variants and an explicit local-change/heating combination,
clock/minute/target rounding boundaries, invalid identities and relationships,
bad inner/outer CRCs separately, malformed/truncated reports, unsupported modes,
missing/future/negative/stale timestamps and signed timestamp limits. A deliberately
different legacy template proves opaque values, `97` bits and `5B` stay preserved
in the retained legacy diagnostic codec. Normal delivery never calls it. Existing state-machine and host regressions still apply.

The normal Build workflow also runs host/HTTP, capacity, discovery-limit, real
broker and pinned RadioLib tests and an ESP-IDF 5.5.1 ESP32-C6 firmware build.
Actual HA integration on the final affected revision is required for normal
command-path changes, including no-template draft-and-Apply control. Host/worker
suites also exercise generated command routing, retry context, legacy preservation,
time, recovery, replay protection and listen-only silence.

Software vectors establish construction correctness only. Independent on-air
capture, radiator readback and physical heating acceptance remain unverified
until hardware testing. A CRC-valid frame does not establish delivery or heating.
