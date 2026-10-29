# Basic Home Assistant interaction

Use HA's built-in **MQTT, template, input_number, input_select and script**
integrations. No custom integration, discovery publisher or additional service
runs in an installation. Scheduling, notifications and history stay in HA.

The complete [package](../examples/home-assistant/cleverbox.yaml) configures two
explicit radiator IDs. The [dashboard card](../examples/home-assistant/dashboard.yaml)
shows measured temperature, observed target/mode, freshness, draft controls,
Apply, and separate last admission/delivery results. The deliberate tradeoff is
one extra **Apply** click: edits cannot silently copy an old observation into
another setting, and a thermostat card cannot imply unsupported heating activity.
No climate/HVAC-action entity or inferred heating/idle signal is supplied.

## Setup

1. Pair each radiator in Cleverbox's HTTP page. Obtain the stable decimal IDs
   from `GET /api/radiators` and the bridge's 12-character MAC identity from
   `GET /api/mqtt`. Names do not route commands. Set the timezone, establish device
   time and explicitly save each radiator's learned/imported controller template.
   These prerequisites remain necessary after adopting MQTT; see
   [time](time.md), [registry](registry.md) and [controller state](controller-state.md).
2. Configure the LAN broker in Cleverbox's HTTP page. Add HA's **MQTT** integration
   under **Settings → Devices & services**, using the same broker. Use a protected
   trusted LAN and appropriate broker ACLs; Cleverbox currently uses TCP without TLS.
   HA needs read access to `cleverbox/v1/<bridge_id>/#` and write access to
   `cleverbox/v1/<bridge_id>/radiators/+/set`. Keep command retention off.
3. Copy `examples/home-assistant/cleverbox.yaml` into HA's `packages` directory.
   Replace **every** `020000000001` with your bridge identity. Replace IDs `1` and
   `2` throughout the package (topics, entity references, helper keys, script
   selector and allowed-ID list). The sample IDs and addresses are synthetic.
   Duplicate the per-radiator blocks and extend that list for more radiators.
4. Merge this into HA's `configuration.yaml` (preserve any existing `homeassistant`
   configuration):

   ```yaml
   homeassistant:
     packages: !include_dir_named packages
   ```

   Existing packages must not define the same keys. Keep the package's dictionary
   style `mqtt: sensor:` / `binary_sensor:` for package merging.
5. Run HA's configuration check, restart HA, then paste the dashboard example
   into a manual card. Replace its entity IDs to match the package. If HA entity
   IDs were customized or already existed, update all package/dashboard references
   to the actual IDs. The package has no startup settings automation.

## Using the controls

**Draft target** and **Draft mode** are local HA inputs. Editing either transmits
nothing; the other draft stays as displayed. Set both intentionally, then press
**Apply**. Both draft values are sent together. Off sends `mode:1, centi_c:0`;
the draft target stays visible for the next non-Off Apply. Comfort/Eco/Frost send
the displayed 5–30 °C target. Drafts reset to 21 °C/Comfort on HA startup and
never initialize themselves from reports. They are labelled drafts, not requested
or confirmed state. The bridge/codec quantization is unchanged.

Apply captures the current connection session and single-use `command_token`
from the radiator snapshot, publishes once at QoS 0 with `retain:false`, and
never retries. No HA counter, wall-clock ID, persistent allocator, floor read/
increment or restart synchronization is needed. The bridge atomically consumes
that token and allocates the next bridge-wide numeric request ID. Existing
numeric clients remain compatible; see the [command contract](mqtt.md#home-assistant-single-use-command-tokens).

Rapid Apply actions may use the same token: only one can consume it. Later
attempts receive `invalid_command_token`. After a refreshed snapshot, an Apply
while RF is pending receives `busy_or_unavailable_or_missing_template`. Both
rejections leave the admitted command alone. There is no queue of later settings
for that radiator; another radiator can proceed independently. Wait for delivery
and inspect the drafts before explicitly applying again. Do not add an automatic
retry automation: rejected, stale or busy drafts must not become delayed settings.

HA schedules can set the two helpers and press Apply using ordinary services:

```yaml
sequence:
  - action: input_number.set_value
    target:
      entity_id: input_number.cleverbox_1_draft_target
    data:
      value: 20.5
  - action: input_select.select_option
    target:
      entity_id: input_select.cleverbox_1_draft_mode
    data:
      option: Comfort
  - action: button.press
    target:
      entity_id: button.cleverbox_1_apply
```

Keep automation writes to the same drafts serialized in HA if they compete with
each other. A manual user can edit those drafts too; Apply sends the values
visible at execution, not a cached value from a different UI action.

## Availability and failures

- **Ready** is transport readiness after successful command SUBACK, not radiator
  reachability. Apply also requires nonexpired bridge/radiator snapshots with the
  same session. Broker/bridge/HA restarts refresh that session/token context
  automatically, without sending settings. Any old payload is rejected. An
  admitted RF exchange continues during HA/broker/network loss; a bridge reboot
  abandons it and starts idle, without replay.
- Observation sensors require a received report, the same ready connection,
  report age below five minutes and a snapshot received within 15 seconds.
  HA's native `expire_after` handles missing snapshots; a one-second HA template
  timer adds local elapsed time to reported age between snapshots. Missing
  temperature/target values remain unavailable. A saved template is never an
  observation. Apply remains usable with stale/missing reports when transport is
  ready, because the normal RF path polls for a new report.
- **Last admission** shows `accepted` or a rejection reason. **Last delivery**
  separately shows the last tracked phase (`waiting_report`, `waiting_readback`,
  `matched`, `timeout`, `error`, etc.). Their `result` attributes include the
  original session, allocated request ID, token and error/details, with a local
  `received_at` timestamp. They are historical result displays, including across
  HA restart; a result from an earlier session does not prove current reachability.
  HTTP may later have controlled the same radiator. No result is copied into
  observed sensors, and one stage does not erase the other stage's failure.
- `time_unavailable`: establish device time. Missing template/busy/unavailable
  share the existing admission reason; inspect the radiator's HTTP status for
  template availability, pending exchange and radio health. `timeout` means no
  required report arrived within the existing bounded RF deadline. `matched`
  means a later matching radiator report, not causality or heating output.
- MQTT remains best effort: there is no result history/backlog or exactly-once
  guarantee. HA could miss a rejection during its own outage. The latest admitted
  MQTT delivery is republished on bridge reconnect with its original correlation;
  a bridge reboot loses that transient result. Never infer success from silence.

## Reproduce validation

On Linux with C/C++17, Make, Node.js, `libcjson-dev`, `pkg-config`, Mosquitto 2.x,
Python 3.14 and a fresh checkout:

```sh
python3.14 -m venv build/ha-venv
build/ha-venv/bin/pip install -r host/requirements-ha.txt
PATH="$PWD/build/ha-venv/bin:$PATH" make test-home-assistant
make test test-http test-capacity
PATH="$PWD/build/ha-venv/bin:$PATH" make test-mqtt-broker
# Separately, in the pinned ESP-IDF 5.5.1 environment:
(cd firmware && idf.py set-target esp32c6 && idf.py build)
```

`test-home-assistant` runs **HA Core 2026.10.4**, validates the exact package with
HA's configuration checker, then starts real HA subprocesses, a loopback-only
Mosquitto process and the production host simulator. HA's actual MQTT config
flow persists broker setup, and actual helper/button service calls publish the
commands. A JSON-lines test driver only invokes services/reads HA state; it does
not replace integrations/entities or mock the broker. No household credentials
are needed. Set `CLEVERBOX_HA_TEST_DIR` to a fresh directory to retain test logs;
CI uploads the configuration-check, HA and broker logs on success or failure.

The suite covers two radiators, complete target/mode changes, rapid actions,
busy rejection, missing time/template, stale/missing observations, RF timeout
and recovery, separate results, actual HA/bridge process restarts and broker
restart with RF completion offline. Reconnect-only steps assert no RF changes.
RF deadline/age tests use the simulator's test clock; the 15-second HA snapshot
expiry uses real elapsed time. Browser rendering and HA OS/Supervisor installation
are not part of this Core/service test. The optional real-socket host adapter can
also be started interactively with `python host/serve.py --real-mqtt`; configure
the broker through the normal HTTP page, and use the simulated Wi-Fi events in
test-clock mode for automated tests.

The local Work sandbox can install HA and validate the package, but blocks
`ifaddr.get_adapters()`/`getifaddrs` with `PermissionError: [Errno 1] Operation not
permitted`. This prevents HA's network/HTTP/MQTT startup. Do not count that local
attempt as actual HA interaction passing; the CI test runs unmodified HA on a
normal Linux runner. Broker-only/fake-client suites are separate evidence.
Physical Wi-Fi, ESP-MQTT and RF/heating/flash acceptance remain pending regardless
of software results. See [MQTT board acceptance](mqtt.md#validation-and-board-acceptance).

Official documentation consulted for this design: [MQTT setup](https://www.home-assistant.io/integrations/mqtt/),
[packages](https://www.home-assistant.io/docs/configuration/packages/),
[templates](https://www.home-assistant.io/integrations/template/),
[input number](https://www.home-assistant.io/integrations/input_number/),
[input select](https://www.home-assistant.io/integrations/input_select/),
[scripts](https://www.home-assistant.io/integrations/script/),
[MQTT sensors and expiry](https://www.home-assistant.io/integrations/sensor.mqtt/)
and [MQTT climate](https://www.home-assistant.io/integrations/climate.mqtt/).
Discovery and usability polish follow in a separate focused PR; Matter is optional later.
