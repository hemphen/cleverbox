# Implementation plan

## Product behaviour

The ESP32-C6 runs the complete application. The CC1101 supplies bidirectional
radiator RF; Wi-Fi carries Matter to the home controller. Home Assistant records
received readings and runs schedules and automations. Cleverbox holds current
device state and persistent inclusion data, not historical readings.

Use Espressif ESP-IDF + ESP-Matter in C/C++. Select and pin a compatible release
pair at the first firmware milestone, with a lockfile and reproducible build
instructions. Do not add a Python daemon or a Node.js bridge to the appliance.

Start with one Matter bridge and one heating thermostat endpoint. Expand to
multiple endpoints only after the one-radiator path works. A stable radiator
identity maps to a stable endpoint, persisted across power cycles.

| Matter-facing value | Implementation rule |
| --- | --- |
| Local temperature | Radiator measurement; unavailable sentinel remains unavailable |
| Heating target | Translate units and enforce implemented bounds; track requested and observed values separately |
| System mode | Expose only implemented radiator mappings; do not equate Eco, Comfort and Auto without an explicit mapping |
| Reachability | Based on radio liveness, not merely Wi-Fi connection |
| Heating activity | Omit optional reporting until supported by the reference and implementation |

Use the public [RF specification](https://github.com/hemphen/clevertouch-protocol/blob/main/docs/radiator-rf/specification.md)
and [shared values](https://github.com/hemphen/clevertouch-protocol/blob/main/docs/common.md)
as the implementation contract. Record the source commit used when adding the
codec and tests, without copying the specification into this repository.

Matter command handling must distinguish acceptance for delivery from confirmed
radiator state. Bound retries and reconcile later reports; avoid claiming that
a setpoint write proves heating output changed.

## Small implementation milestones

1. **Build and board bring-up.** Establish the sourced pin map, pinned SDKs,
   build/flash/monitor commands and serial diagnostics. Read the CC1101 identity
   and receive frames. Add a firmware build check once a real build exists.
2. **Radiator communication.** Implement framing, checksums, decoding, controller
   messages and the documented inclusion exchange. Store pairing state. Use
   synthetic reference vectors for codec tests. Demonstrate a target change on
   one radiator with the Touch E3 disconnected and observe its resulting state.
3. **Matter integration.** Commission over Wi-Fi into HA; expose temperature and
   heating target, then the supported modes. Persist Matter fabric and endpoint
   identity. Keep radio work outside the Matter event loop.
4. **Useful appliance.** Add explicit inclusion/removal and factory-reset controls,
   documented button behaviour and status indication. Test two radiators,
   individual addressing, power cycles and unavailable devices.
5. **Installation and endurance.** Write the actual installation steps, publish
   a buildable firmware version, and test a sustained run with HA/network outages,
   reconnects and repeated commands. Add upgrade handling once the storage layout
   is established; do not reset pairing as a side effect of normal updates.

Each milestone should end in a small demonstrable result. No placeholder features
or broad framework are needed to start.

## Acceptance for the first prototype

- HA discovers one bridged thermostat after commissioning.
- Its measured temperature follows radiator reports.
- A target change reaches that radiator and its observed state converges.
- Unavailable/stale data is not shown as fresh or confirmed.
- Restarting Cleverbox retains pairing and the same Matter endpoint.
- HA and Wi-Fi loss do not trigger arbitrary new setpoints or inclusion.
- Continued heating behaviour without HA and without Touch E3 is observed and
  described accurately; thermostat ownership is not invented by this project.
- No provider account, cloud access or separate radiatord process is required.

Passing software tests does not imply that these physical checks have passed.

## Later, only when needed

Matter-over-Thread can be an alternative firmware/configuration on this board.
Do not implement simultaneous Wi-Fi/Thread operation in the first version.
A local HTTP API can reuse the same command/state handling if a concrete client
needs it. Neither feature is necessary for the first HA-controlled appliance.

Weekly schedules belong in HA initially. Power estimates, vendor diagnostics,
onboard history, other board support and general-purpose protocol plugins are
not part of the first product.
