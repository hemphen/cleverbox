# Instructions for contributors and coding agents

Cleverbox is firmware for one appliance: a local radiator bridge on the Elecram
ESP32-C6 + CC1101 board. Read [README.md](README.md) and
[docs/architecture.md](docs/architecture.md) first.

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
- Never use synthetic test-vector values as defaults
  in physical firmware.
- The explicitly selected Touch E3 replacement-controller codec profile is
  permitted. Its constants must trace to observed protocol evidence, not to
  synthetic fixtures. Keep its reversible assumptions and input contract in
  [docs/controller-profile.md](docs/controller-profile.md). Captures are evidence,
  not controller personalities or capability descriptions. Normal commands use
  only this generated profile, with explicit non-Boost duration 120 minutes.
  Do not restore a template-based controller stack, capture, import or adoption.

## Scope

- C/C++ with ESP-IDF 5.5.1 and RadioLib. Pin every dependency.
- Interfaces: local HTTP, MQTT and Home Assistant discovery. OTA is planned as
  a separate milestone. No Matter, Thread or Zigbee, and no resources reserved
  for them.
- No scheduler or schedule storage. Home Assistant owns schedules. Cleverbox
  accepts immediate target and mode commands only.
- Build only what the current milestone needs. No abstractions for other boards,
  transports or vendors.
- MIT license for project code.

## Design rules

- One protocol codec, independent of RTOS and chip code and testable on a host.
  HTTP and MQTT share the same admission and delivery code.
- Keep requested settings separate from reported state. A transmitted frame or
  an ack is not confirmation.
- Report stale or unavailable data as such. Do not invent heating activity or
  energy figures.
- Radiator IDs are stable across restarts and never reused.
- Nothing is sent at boot: no inclusion, polls or settings changes.
- Persist registry v2 only: identities, names and saved relationships. There are
  no deployed devices requiring older registry formats or template migration.
- Use bounded queues, timeouts and retries. Do not block network or event loops.
- Do not add a thermostat or PID loop without an explicit requirement.
- Take pin assignments from the board schematic or manufacturer firmware and
  record the source and board revision. Do not guess.

## Host simulation

The simulation must run the production receive and state-machine code. Expected generated frame vectors
stay explicit in tests; no synthetic original-controller injection workflow.

## Tests

- Test the codec with synthetic vectors from the public specification.
- Test state transitions and error paths that affect command delivery.
- State exactly what was built, what was tested in software and what was tested
  on hardware. Do not describe planned behaviour as working.

## Repository hygiene

- Never commit credentials, real household addresses, pairing codes, NVS dumps
  or RF captures.
- Do not copy the protocol specification or general ESP-IDF documentation into
  this repository.
- Make focused commits. Documentation describes current behaviour, not the
  history of changes. Remove abandoned alternatives.
- Write documentation in short, plain sentences. Put each fact in one place and
  link to it rather than repeating it.

## CI

The `Build` workflow runs the unit, host and broker tests and builds the
firmware on pull requests and `main`. The `Home Assistant integration` workflow
runs weekly on `main`, on manual dispatch, and when a pull request gets the
`run-ha` label. Re-apply the label to rerun it after new commits.

For changes to MQTT commands, sessions or state publishing, discovery or the HA
package, registry identity or observations, time or network recovery, or the
host HA code and tests: request `run-ha` on the final revision and wait for the
result before claiming Home Assistant validation. A green `Build` does not
cover Home Assistant.

No approval is needed for routine implementation choices within this scope.
