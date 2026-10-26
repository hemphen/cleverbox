# Cleverbox

A small, local Matter bridge for Purmo/LVI radiators, built specifically for the
Elecram Sub-GHz 855–925 MHz Bridge (ESP32-C6 + CC1101).

**Status: implementation plan. No firmware is implemented or hardware-tested here yet.**

Cleverbox will translate Matter thermostat commands into radiator RF messages
and report radiator state back over Wi-Fi. Home Assistant supplies dashboards,
history, schedules, automations and notifications through its standard Matter
integration. No Touch E3, provider account or cloud service is intended to be
required in normal operation.

## First useful version

- One Elecram board, powered through USB-C, connected to the home Wi-Fi.
- Local inclusion and control of one radiator, then multiple radiators.
- A Matter bridge with a stable thermostat endpoint for each radiator.
- Measured temperature, target setpoint and only the operating modes we implement.
- Persistent pairing and settings; honest availability and command state.
- Commissioning into Home Assistant. No Thread border router required.

The radiator protocol remains proprietary; Matter is the standard interface
towards the home. A host running radiatord is not required by the intended product.

## Scope

This is one embedded appliance, not a general radio gateway or a protocol
research project. We use the existing specifications as working truths and
implement them. Hardware acceptance tests check our implementation; they are not
a prerequisite programme to re-establish every protocol claim.

Thread is a possible later alternative on the same hardware, not part of the
first build. Ethernet/PoE, Z-Wave, other boards, cloud fallback, an onboard history
database and a schedule engine are outside this implementation. A local HTTP
control interface may be added later if useful; it is not required for Matter.

## Working here

Read [AGENTS.md](AGENTS.md), the [hardware checklist](docs/hardware.md),
[implementation plan](docs/implementation.md) and [libraries](docs/libraries.md).
The [firmware directory](firmware/README.md) describes the build layout.

Protocol definitions live in
[clevertouch-protocol](https://github.com/hemphen/clevertouch-protocol/tree/main/docs/radiator-rf).
Existing decoder and tooling live in
[clevertouch-radio](https://github.com/hemphen/clevertouch-radio).
Link to these sources rather than copying their documentation or private captures.

Licensed under [MIT](LICENSE).
