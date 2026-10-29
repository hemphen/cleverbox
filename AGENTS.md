# Instructions for agents and contributors

## Build this appliance

Cleverbox implements a local radiator bridge targeting the Elecram
ESP32-C6 + CC1101 Sub-GHz 855–925 MHz board. Keep changes specific to that
hardware and purpose, with a small opt-in host simulation. Read README.md,
docs/implementation.md and docs/libraries.md first.

Treat the protocol descriptions in hemphen/clevertouch-protocol as the working
contract. Use hemphen/clevertouch-radio for implementation references when needed.
Do not start a protocol audit, repeat reverse engineering, or block implementation
because a documented finding is provisional. We are building a useful tool based
on believed truths, not evaluating the research project.

If a necessary detail is unspecified, choose the smallest explicit, reversible
implementation assumption and document it beside the relevant code. Do not invent
a claim that the reference specifies it. If an actual implementation test produces
contradictory evidence, record a concise finding and refer protocol investigation
back to the source project. Do not expand this repository into a research archive.

## Keep the repository small

- Do not duplicate protocol specifications, captures, conversation history or
  general Matter/ESP-IDF documentation.
- Use C/C++, ESP-IDF and RadioLib for firmware. Keep the host adapter small.
  Add MQTT/HA discovery after setup/network milestones; Matter is optional later.
  Pin dependencies when introduced. Use MIT for project code.
- Build only components required by the current milestone. Avoid speculative
  abstractions for other hardware, transports or providers.
- Use the libraries and component boundaries in docs/libraries.md. Keep one
  protocol codec, independent of chip/RTOS code and testable on a host.
- Never commit credentials, Matter fabric secrets, real household addresses,
  pairing codes, NVS dumps or private captures.
- Make focused commits. Update documentation for resulting behaviour, not the
  chronology of attempts. Remove abandoned alternatives.

Build and test local HTTP, the persistent registry and independent per-radiator
delivery before home Wi-Fi/device time and MQTT with HA discovery. Keep Matter optional later.
All interfaces must use the same protocol component and command handling.
Host simulation must reuse production receive/learning and state machines; keep
synthetic controller fixtures explicit and separate from radiator reports. Never
use synthetic 97/5B values as physical firmware defaults. Controller state is
persisted only by explicit save/import actions; passive learning never writes flash.

## Scheduling

Do not implement a scheduler, weekly-program storage or Matter schedule execution
in Cleverbox. Scheduling belongs in HA because it is controller-side, not native
to the radiators. Cleverbox accepts immediate target/mode commands. Reconsider
only if the external protocol reference establishes a native radiator scheduling
operation; even then, expose that operation rather than execute schedules here.

## Implementation and validation

- Do not guess the board's pin mapping. Obtain it from the board schematic or
  manufacturer firmware and record its source and board revision.
- Separate requested settings from radiator-reported state. A transmitted packet
  or ack-shaped reply alone must not be labelled confirmed application.
- Preserve stable radiator identities across restarts. Report stale or
  unavailable state honestly; do not invent heating activity or energy readings.
- No automatic inclusion or unsolicited settings changes at boot.
- Use bounded queues, timeouts and retries; avoid blocking network/event loops.
- Test codec behaviour with synthetic vectors from the public specification.
  Test state transitions and error paths that affect command delivery.
- State exactly what was built, tested in software and tested on hardware.
  Do not describe planned behaviour as already working.
- Do not assume the location of the radiator thermostat control loop. Implement
  the reference behaviour and check continued operation during HA/network loss.
  Do not add a new PID controller without an explicit requirement.

No approval is required for routine implementation choices within this scope.
