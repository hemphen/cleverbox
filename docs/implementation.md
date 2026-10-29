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
   these paths with two deterministic radiators and explicit synthetic controller
   fixtures; see [simulation](simulation.md). Hardware acceptance remains pending.
2. **Saved one-radiator setup.** Explicitly save learned state or import-and-save
   a validated same-radiator frame. Implemented with a separate versioned NVS
   record, preserving existing pairing. Restart restores configuration only;
   delivery still requires a fresh report. See [storage policy](controller-state.md).
3. **Persistent registry with serialized delivery.** Implemented: bounded capacity,
   stable identities/names, explicit HTTP routing, inclusion/removal, per-radiator
   observations/results and legacy migration. One pairing or command exchange
   globally; other exchanges return busy. See [registry](registry.md).
4. **Independent per-radiator delivery.** Implemented: one outstanding command per
   radiator, independent report deadlines/results and round-robin selection of ready
   TX frames on the single CC1101. Pairing remains exclusive; there is no second
   command queued for a busy radiator. See [registry](registry.md).
5. **Persistent home Wi-Fi and recovery.** Implemented: local HTTP provisioning,
   save after DHCP, previous-setup retention, protected serial-password recovery AP,
   bounded attempts/paced reconnect and network-only forget. Storage errors trigger
   best-effort rollback/readback; unresolved persistence is reported and keeps AP access. Production policy and
   HTTP paths are software-tested; physical Wi-Fi/RF acceptance remains pending.
   See [networking](networking.md).
6. **Device-managed time.** Add a clock with explicit timezone behavior. Current
   commands still use the browser clock; no schedules or networking-triggered commands.
7. **MQTT and Home Assistant discovery.** Expose measurements, targets, supported
   modes and honest availability through the existing command handling. Test
   network loss/reconnect without arbitrary settings changes.
8. **Installation and endurance.** Validate physical pairing/control, power cycles,
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
