# Instructions for contributors and coding agents

Cleverbox is firmware for one appliance: a local radiator bridge on the Elecram
ESP32-C6 + CC1101 board. Read [README.md](README.md) and
[docs/architecture.md](docs/architecture.md) first.

## Experimental development

Cleverbox is experimental firmware. The goal now is to get it working on real
hardware and iterate quickly, under controlled conditions with the developer
present. Engineer for that, not for an unattended production appliance.

- Prefer simple, readable code to defensive programming.
- Prefer ESP-IDF, RadioLib, FreeRTOS, ESP-MQTT and cJSON facilities to custom
  infrastructure, when they make the code simpler.
- Prefer straightforward control flow to state machines, and a visible error
  with manual retry or restart to automatic recovery.
- Implement the actual requirement, not speculative future needs. Do not add
  abstractions to make code look architecturally complete.
- Deleting code is welcome. Do not keep complexity because work went into it.
- A few meaningful tests beat exhaustive fault-injection matrices.
- Production hardening is a later milestone. Add new accepted risks to
  [docs/production-hardening.md](docs/production-hardening.md) instead of
  defending against them now.

These remain correctness requirements, not optional hardening:

- Correct RF frame construction and CRCs; correct pairing and command sequencing.
- RF is transmitted only on an explicit pairing or command request. Nothing is
  sent at boot, on reconnect or on recovery, and retained MQTT commands are rejected.
- Commands go only to the intended paired radiator.
- Requested settings stay separate from reported values. A transmitted frame or
  an ack is not confirmation.
- Bounds checks that prevent memory corruption; locking where tasks share
  mutable state; timeouts where a block would stop normal operation.

## Protocol

- Treat [clevertouch-protocol](https://github.com/hemphen/clevertouch-protocol)
  as the specification. Use [clevertouch-radio](https://github.com/hemphen/clevertouch-radio)
  as implementation reference.
- Do not re-derive or audit the protocol here, and do not block work because a
  finding is marked provisional.
- When the specification leaves a detail open, choose the smallest reversible
  assumption and document it next to the code. Do not claim the specification
  defines it.
- When hardware contradicts the specification, record a short finding and take
  the protocol question to clevertouch-protocol.
- Never use synthetic test-vector values as defaults in physical firmware.
- Normal commands use only the generated Touch E3 replacement-controller
  profile, with explicit non-Boost duration 120 minutes. Its constants must
  trace to observed protocol evidence. Keep its assumptions and input contract
  in [docs/controller-profile.md](docs/controller-profile.md). Do not restore a
  template-based controller stack, capture, import or adoption.

## Scope

- C/C++ with ESP-IDF 5.5.1 and RadioLib. Pin every dependency.
- Interfaces: local HTTP, MQTT and Home Assistant discovery. OTA is a separate
  milestone. No Matter, Thread or Zigbee.
- No scheduler or schedule storage; Home Assistant owns schedules. Cleverbox
  accepts immediate target and mode commands only.
- No thermostat or PID loop without an explicit requirement.
- No abstractions for other boards, transports or vendors.
- Take pin assignments from the board schematic or manufacturer firmware and
  record the source and board revision. Do not guess.
- There are no deployed devices. Stored formats may change without migration.
- MIT license for project code.

## Tests

- The host simulation must run the production receive, pairing and command
  code. Expected generated frames stay explicit vectors in tests.
- Test the codec with synthetic vectors from the public specification.
- Test real functionality and the essential error paths; do not add tests for
  deferred defences.
- State exactly what was built, what was tested in software and what was tested
  on hardware. Do not describe planned behaviour as working.

## Repository hygiene

- Never commit credentials, real household addresses, pairing codes, NVS dumps
  or RF captures.
- Do not copy the protocol specification or general ESP-IDF documentation into
  this repository.
- Documentation describes current behaviour, not the history of changes.
- Write documentation in short, plain sentences. Put each fact in one place and
  link to it.

## CI

The `Build` workflow runs the unit, host, broker and RadioLib tests and builds
the firmware on pull requests and `main`. The `Home Assistant integration`
workflow runs weekly on `main`, on manual dispatch, and when a pull request
gets the `run-ha` label. Re-apply the label to rerun it after new commits.

For changes to MQTT, discovery, the HA package or the host HA test, request
`run-ha` on the final revision before claiming Home Assistant validation. A
green `Build` does not cover Home Assistant.

No approval is needed for routine implementation choices within this scope.
