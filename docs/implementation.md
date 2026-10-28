# Implementation plan

## Product behaviour

The ESP32-C6 runs the appliance; the CC1101 supplies bidirectional radiator RF.
Local HTTP is the first setup/control interface. Home Assistant will record
readings and run schedules/automations through MQTT with discovery. Matter is an
optional later interface. Cleverbox holds current device state and saved setup,
not historical readings.

Use C/C++, ESP-IDF and RadioLib. Keep the host simulation small and opt-in; it
runs the same codec, pairing, command and receive handling without board hardware.
No Python daemon or Node.js bridge is required by the intended physical appliance.
Stable radiator identities and requested/observed separation apply to every interface.

Use the public [RF specification](https://github.com/hemphen/clevertouch-protocol/blob/main/docs/radiator-rf/specification.md)
and [shared values](https://github.com/hemphen/clevertouch-protocol/blob/main/docs/common.md)
as the implementation contract. Record the source commit used when adding the
codec and tests, without copying the specification into this repository.

Command handling must distinguish acceptance for delivery from confirmed
radiator state. Bound retries and reconcile later reports; avoid claiming that
a setpoint write proves heating output changed.

## Small implementation milestones

Use the [libraries and component boundaries](libraries.md).

1. **One-radiator HTTP application.** Portable codec, Elecram transport, explicit
   pairing/persistence and report-driven commands are implemented. Passive state
   learning is RAM only; manual imports remain. The host application now exercises
   these paths with one deterministic radiator and an explicit synthetic controller
   fixture; see [simulation](simulation.md). Hardware acceptance remains pending.
2. **Registry and saved setup.** Support multiple radiators with stable identities,
   explicit inclusion/removal and saved setup state. Define learned-state storage
   policy here; simulation does not persist it ahead of that decision.
3. **Home network and device time.** Add home Wi-Fi setup and device-managed clock
   with explicit timezone behavior. Current commands use the browser clock.
4. **MQTT and Home Assistant discovery.** Expose measurements, targets, supported
   modes and honest availability through the existing command handling. Test
   network loss/reconnect without arbitrary settings changes.
5. **Installation and endurance.** Validate physical pairing/control, power cycles,
   multiple devices and continued operation without HA/Touch E3. Publish installation
   and upgrade handling that preserves setup. Matter may be added later if useful.

Each milestone should end in a small demonstrable result. No placeholder features
or broad framework are needed to start.

## Acceptance for the first prototype

- HA discovers radiators through MQTT after the integration milestone.
- Its measured temperature follows radiator reports.
- A target change reaches that radiator and its observed state converges.
- Unavailable/stale data is not shown as fresh or confirmed.
- Restarting Cleverbox retains pairing and stable radiator identities.
- HA and Wi-Fi loss do not trigger arbitrary new setpoints or inclusion.
- Continued heating behaviour without HA and without Touch E3 is observed and
  described accurately; thermostat ownership is not invented by this project.
- No provider account, cloud access or separate radiatord process is required.

Passing software tests does not imply that these physical checks have passed.

## Later, only when needed

Matter over Wi-Fi or Thread remains optional later. Do not add either stack to
the current HTTP/setup milestones.
The local HTTP interface remains available for setup and testing.

Weekly schedules belong in HA. Cleverbox has no scheduler or weekly-program
storage because scheduling is not native to the radiators. Power estimates, vendor diagnostics,
onboard history, other board support and general-purpose protocol plugins are
not part of the first product.
