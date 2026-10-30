# Home Assistant setup

Enable Cleverbox's opt-in **Home Assistant discovery** on the HTTP page after
configuring MQTT. With HA's built-in MQTT integration on the same broker, each
configured radiator appears as a device automatically. No custom integration or
additional publisher is needed. Scheduling, notifications and history stay in HA.

| Automatic entity | Meaning |
| --- | --- |
| Temperature | Measured temperature from a received report, when present/fresh |
| Observed target / mode | Reported settings, never draft/requested settings |
| Observation fresh | Received report with conservative freshness headroom |
| Report age | Age in seconds at the last snapshot; diagnostic, not a heartbeat |
| Last admission | Latest MQTT admission or rejection this connection; `result` attributes |
| Last delivery | Latest tracked MQTT RF delivery; original correlation in `result` |

Discovery does **not** create controls. The optional [draft/Apply package](../examples/home-assistant/cleverbox.yaml)
adds local target/mode drafts, session/token snapshots and explicit Apply buttons.
MQTT number/select/climate entities publish settings independently; a built-in
button's command template cannot read another entity's current token and both
drafts. They cannot implement this complete single-use command safely by
configuration alone. Firmware holds no draft settings just to remove YAML.
No heating/idle, power, energy or climate/HVAC-action entity is invented.

## Setup

1. Pair and name radiators through Cleverbox HTTP. Configure the LAN broker and
   add HA's **MQTT** integration under **Settings → Devices & services**.
   Use the default discovery prefix `homeassistant` and birth topic
   `homeassistant/status` with payload `online`. Custom prefixes/topics are not
   configurable in this milestone. Cleverbox defaults to discovery **disabled**.
2. Press **Enable HA discovery** under MQTT broker. Observation devices appear
   without any YAML. Names label devices; renaming does not change identities.
   For diagnostics inspect **Settings → Devices & services → MQTT**.
3. For controls, first establish device time/timezone and explicitly save each
   learned/imported controller template. These are the existing prerequisites:
   [time](time.md), [registry](registry.md), [controller state](controller-state.md).
4. Copy the optional package into HA's `packages` directory. Replace every
   `020000000001` with `GET /api/mqtt`'s bridge identity. Replace IDs `1` and `2`
   in topics, helper/script keys, references, selector and allow-list with
   `GET /api/radiators`' stable decimal IDs. Duplicate those blocks for more
   radiators. The package contains no manual observation entities to duplicate
   discovery. Existing customized helper/button IDs must be preserved.
5. Merge into `configuration.yaml`, preserving existing HA configuration:

   ```yaml
   homeassistant:
     packages: !include_dir_named packages
   ```

   Run HA's configuration check and restart. Paste the [dashboard example](../examples/home-assistant/dashboard.yaml)
   into a manual card, replacing bridge/radiator IDs and any customized entity IDs.
   Observation IDs include bridge identity; package helper/Apply IDs retain their
   previous names. There is no startup command automation.

MQTT currently uses TCP without TLS on a protected trusted LAN. The bridge account
needs read access to its `/radiators/+/set` topics and `homeassistant/status`,
write access to `cleverbox/v1/<bridge_id>/#` and **only its own**
`homeassistant/device/cleverbox_<bridge_id>_+/config` topics (express the per-ID
restriction using the broker's ACL facilities; `+` cannot wildcard part of an MQTT
level). HA needs discovery/state read access and command-topic write access.
Keep command retention off. No wildcard broker delete is used.

## Discovery lifecycle and persistence

`POST /api/mqtt {"action":"discovery_enable"}` / `discovery_disable` persist the
opt-in separately from broker credentials. GET reports `discovery_enabled`,
`discovery_pending` (outstanding configuration/cleanup work), and `discovery_error`.
202 queues an action; poll status for completion. Turning discovery off removes
its automatic entities but retains pairing, templates, MQTT and draft controls.
Disable the package separately if you also want to remove its helpers/buttons.

Each radiator uses one retained MQTT **device discovery** configuration at
`homeassistant/device/cleverbox_<bridge_id>_<registry_id>/config`. Device identifier
`cleverbox_<bridge_id>_<registry_id>` and each component's unique ID use the full
stable MAC bridge identity and registry ID, never a name or registry slot.
Default HA entity IDs also include bridge identity to avoid cross-bridge clashes.
HA user overrides persist according to its registry rules. Rename updates the
device's supplied name; an explicit HA user name continues to take precedence.

- Initial enable/addition journals the topic **before** publication. Renames update
  the same config. Registry changes are checked at a five-second bounded cadence.
- HA birth, broker reconnect and bridge restart replay current configs and fresh
  snapshots. Retained configs also allow HA to start while the bridge is offline;
  entities remain unavailable without current state. Identical configs are not
  republished every heartbeat. None of these paths calls RF command admission.
- Removal/disable replaces the config at exactly the journaled topic with HA's
  documented **component-removal** payload (each component contains only its
  platform). These small retained cleanup markers intentionally remain on the
  broker, containing no active entity definitions. A zero-byte retained deletion
  can be missed while HA reconnects, leaving old unavailable entities in its
  registry. Retained markers replay cleanup when HA resubscribes. A broker QoS-1 PUBACK is required before removing its device-side journal entry.
  Unrelated retained broker data is untouched. Re-enable replaces the marker for
  a still-configured ID with the active config; re-pairing allocates a new ID. HA removes devices once no other
  entities/references remain; optional package helpers are independent.
- Offline removal/disable survives restart and retries when this broker becomes
  reachable. If MQTT itself is disabled, Wi-Fi is down or command SUBACK is
  rejected, cleanup waits; re-enable MQTT with the same credentials. A broker
  change/forget is refused while any discovery records remain: disable discovery,
  reconnect to the **old** broker and wait for `discovery_pending:0` first.
- One confirmed discovery publication is in flight. Every disconnect, immediate
  send failure, or missing PUBACK after five seconds destroys the client/outbox.
  The owner recreates it after a five-second pause; ESP-MQTT automatic reconnect
  is disabled so the old outbox cannot retransmit before the owner runs. Loss is
  latched even if a connection callback follows before the next owner tick. A failed send may
  already be queued inside ESP-MQTT; discarding that outbox prevents an old active
  config from overtaking a later cleanup marker. Commands already admitted continue
  independently. MQTT 3.1.1 PUBACK proves broker receipt, not that HA processed it
  or that a broker ACL accepted a write; verify broker ACLs and HA logs.
- The persistent journal holds twice the configured radiator capacity, not an
  unbounded removal history. A full journal delays new discovery until cleanup
  frees a slot and reports pending work. It never forgets an old topic to advertise
  an untracked one. Record version/size/corruption errors halt discovery and stay
  visible; they never reset radiator setup or overwrite an unreadable journal.
- `cleverbox/ha_discovery` stores a version, opt-in, monotonically changing write
  generation and bounded IDs. It stores no reports, commands or tokens. Writes
  happen only for explicit opt-in changes and topic-journal changes, with new-handle
  readback; a changed generation forces same-setting retries through NVS. Failed
  writes halt discovery until an explicit retry/restart, reporting uncertainty via
  `discovery_error`; reboot may load the old or new record. The journal is never
  erased before replacement. Already acknowledged cleanup may safely replay. Broker persistence must retain
  cleanup markers across broker restarts; deliberately wiping broker data may
  require deleting orphaned HA registry entries manually. Removed IDs leave one
  small broker-side marker each, while device memory/storage remain bounded.

Error codes: `0x7411` discovery PUBACK timeout, `0x7412` serialized payload exceeds
its bound, `0x7413` publication refused by the client boundary; other values are
storage errors. Publication refusal is retried; payload/storage errors require an
explicit opt-in retry or a corrected build/record. The serialized config is at
most 8191 bytes plus NUL. The output buffer is static; ESP-MQTT's bounded outbox
is 9216 bytes for one config plus control traffic. Observation/input limits and
RF behavior remain separate.

## Migrating from PR #12's manual package

Do this once before enabling discovery; keep a copy of customized YAML.

1. Leave discovery off. Replace the old package with the new package, preserving
   your bridge/IDs, helper keys, Apply script/button IDs and allowed-ID list. This
   removes the old template observations/results and raw result sensors while
   retaining the snapshots/session/ready sensors needed by Apply.
2. Check configuration and restart HA. Under entity settings, delete any orphaned
   old observation/result registry entries still left by the template integration.
   Do not delete the retained snapshot, session, ready, draft or Apply entities.
3. Enable discovery. Update dashboards/automations that read observations using
   the mapping below. Controls keep their old IDs and references. HA's template
   and MQTT registries are different platforms: identical unique-ID text does not
   migrate an entity across them or preserve its history automatically.

| Old entity suffix (after `cleverbox_<id>_`) | New automatic entity suffix (after `cleverbox_<bridge>_<id>_`) |
| --- | --- |
| `temperature` | `temperature` |
| `observed_target` | `observed_target` |
| `observed_mode` | `observed_mode` |
| `observation_fresh` (binary sensor) | `observation_fresh` (binary sensor) |
| `last_admission` | `last_admission` |
| `last_delivery` | `last_delivery` |

For example, `sensor.cleverbox_1_temperature` becomes
`sensor.cleverbox_020000000001_1_temperature`. If retaining old observation IDs is
essential, first remove their orphaned registry entries, then rename the new MQTT
entities to those IDs in HA and adjust the supplied dashboard accordingly. Do not
run the old manual observation blocks alongside discovery. There is no automatic
YAML edit or history merge.

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
- Observation sensors require a received report, bridge readiness, a snapshot
  received within 15 seconds and a present measurement. Reports have a five-minute
  stale threshold. Discovery uses **15 seconds of conservative headroom**: values
  become unavailable up to 15 seconds early so HA's local expiry cannot present
  them as fresh beyond that threshold between snapshots. Freshness becomes off
  on missing/stale reports and unavailable on offline/expired snapshots. Saved
  templates and restored configuration never count as reports. Apply remains
  usable with stale/missing reports because RF delivery polls for a new report.
- **Last admission** and **Last delivery** are separate diagnostic snapshots, with
  a `result` attribute containing original correlation/details. They describe
  outcomes, never current reachability or observed settings. Latest admission is
  RAM-only and clears on a new connection session; latest tracked delivery survives
  broker reconnect with its original session but clears on bridge reboot. With no
  outcome they show `none`. They also expire with snapshots/offline transport.
  HTTP may later control the same radiator. One stage never overwrites the other.
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
make test test-http test-capacity test-discovery-limit
PATH="$PWD/build/ha-venv/bin:$PATH" make test-mqtt-broker
# Separately, in the pinned ESP-IDF 5.5.1 environment:
(cd firmware && idf.py set-target esp32c6 && idf.py build)
```

`test-home-assistant` runs **HA Core 2026.10.4**, validates the exact optional control package with
HA's configuration checker, then starts real HA subprocesses, a loopback-only
Mosquitto process and the production host simulator. HA's actual MQTT config
flow persists broker setup, and actual helper/button service calls publish the
commands. A JSON-lines test driver only invokes services/reads HA state; it does
not replace integrations/entities or mock the broker. No household credentials
are needed. Set `CLEVERBOX_HA_TEST_DIR` to a fresh directory to retain test logs;
CI uploads the configuration-check, HA and broker logs on success or failure.

The suite covers automatic device grouping/stable IDs, rename, addition, online
and offline removal/disable cleanup, retained broker data, HA birth/restarts and
two radiators, complete target/mode changes, rapid actions,
busy rejection, missing time/template, stale/missing observations, RF timeout
and recovery, separate results, actual HA/bridge process restarts and broker
restart with RF completion offline. Reconnect-only steps assert no RF changes.
RF deadline/age tests use the simulator's test clock; the 15-second HA snapshot
expiry uses real elapsed time. Browser rendering and HA OS/Supervisor installation
are not part of this Core/service test. The optional real-socket host adapter can
also be started interactively with `python host/serve.py --real-mqtt`; configure
the broker through the normal HTTP page, and use the simulated Wi-Fi events in
test-clock mode for automated tests.

A configuration-check result alone is not runtime evidence. Restricted sandboxes
may prevent dependency installation or HA network-interface discovery; report such
attempts separately. CI runs unmodified HA on a normal Linux runner and uploads
its actual service/broker logs. Broker-only and fake-client suites remain separate
evidence. Physical Wi-Fi, ESP-MQTT and RF/heating/flash acceptance remain pending
regardless of software results. See [MQTT board acceptance](mqtt.md#validation-and-board-acceptance).

Official documentation consulted for this design: [MQTT setup](https://www.home-assistant.io/integrations/mqtt/),
[packages](https://www.home-assistant.io/docs/configuration/packages/),
[templates](https://www.home-assistant.io/integrations/template/),
[input number](https://www.home-assistant.io/integrations/input_number/),
[input select](https://www.home-assistant.io/integrations/input_select/),
[scripts](https://www.home-assistant.io/integrations/script/),
[MQTT sensors and expiry](https://www.home-assistant.io/integrations/sensor.mqtt/)
and [MQTT climate](https://www.home-assistant.io/integrations/climate.mqtt/).
Discovery is implemented. Remaining usability work is installation walkthroughs and
physical UI acceptance, followed by endurance/upgrade preservation and hardware
acceptance. Matter remains optional and deferred.
