# Implementation plan

## Product behaviour

The ESP32-C6 runs the appliance; the CC1101 supplies bidirectional radiator RF.
Local HTTP is the first setup/control interface. Home Assistant will record
readings and run schedules/automations through MQTT, initially with explicit
configuration and then discovery. Matter is an
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
3. **Persistent registry.** Implemented: bounded capacity, stable identities/names,
   explicit HTTP routing, inclusion/removal, per-radiator observations/results and
   legacy migration. Delivery is independently reserved per radiator as described
   in milestone 4; pairing remains exclusive. See [registry](registry.md).
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
6. **Device-managed time.** Implemented: asynchronous ESP-IDF SNTP, explicit
   persistent POSIX timezone (UTC0 default), manual UTC bench setup and honest
   source/sync-age status. Normal commands snapshot device local time and advance
   it monotonically across delivery/retries; unset boot rejects them. No timestamps
   or commands are restored, and time/network events never send settings. See
   [time policy and validation](time.md). Physical time/RF acceptance remains pending.
7. **MQTT transport.** Implemented: explicit persistent broker setup, versioned
   topics, session-bound immediate commands through device-time admission,
   correlated results and separate observations/availability. No RF replay on
   network loss/reconnect or boot. See [MQTT contract](mqtt.md).
8. **Basic Home Assistant interaction.** Implemented: manual built-in HA package
   for explicit stable radiator IDs, complete draft/Apply controls, single-use
   session-bound tokens with bridge-side ID allocation, honest observation expiry
   and separate results. Reproducible actual HA Core/broker/simulator tests are
   separate from fake-client and broker-only tests. See [HA setup](home-assistant.md).
9. **Discovery and setup polish.** Implemented: opt-in persistent MQTT device
   discovery, stable radiator devices/observation and diagnostic entities, honest
   snapshot expiry and bounded durable cleanup on rename/removal/disable/restarts.
   The smaller optional package retains explicit complete draft/Apply controls;
   migration is documented. Remaining usability: a fresh-install walkthrough,
   physical browser/mobile acceptance and checking names/error recovery with real
   users. No firmware drafts or custom HA integration are needed for this stage.
10. **Endurance/upgrade and hardware acceptance.** Validate long running operation,
   configuration retention across upgrades/power cycles, physical pairing/control,
   multiple devices and operation without HA/Touch E3. Record RF/heating behavior,
   task/heap high-water marks and actual Wi-Fi/broker recovery.
11. **Optional Matter.** Consider only after the MQTT/HA path and hardware
   acceptance are useful and reliable. Reuse existing admission/delivery.

Each milestone should end in a small demonstrable result. No placeholder features
or broad framework are needed to start.

## Acceptance for the first prototype

- HA discovers radiator observations automatically when opted in, and the small
  control package applies complete settings to explicit stable radiator IDs.
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
