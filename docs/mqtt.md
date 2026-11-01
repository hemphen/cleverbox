# MQTT

Cleverbox connects to one MQTT 3.1.1 broker over plain TCP. Configure it on the
web page or with [`POST /api/mqtt`](http-api.md#post-apimqtt). Commands sent over
MQTT go through the same admission and delivery as HTTP commands. Pairing and
usable device time must be established first. Commands use the generated
controller profile and never require or accept a template. Listen-only mode (the fresh default)
rejects every command through shared RF admission. Only an explicit STA HTTP
action can enable control; MQTT/network reconnects never change mode.

## Connection

| Setting | Value |
| --- | --- |
| Client ID | `cleverbox-<bridge_id>` |
| `bridge_id` | Station MAC address, 12 lowercase hex digits |
| Session | Clean session, keepalive 30 s |
| Last will | `offline` on the availability topic, retained, QoS 1 |
| Reconnect | Cleverbox destroys the client after any disconnect and creates a new one 5 s later |

After connecting, Cleverbox publishes retained `offline`, subscribes to the
command topic at QoS 0, and waits up to 5 s for the SUBACK. Only then does it
publish `online`, create a new session and accept commands (`connected: true`
in `GET /api/mqtt`). A rejected or late SUBACK closes the client and retries 5 s
later.

## Topics

`B` = `cleverbox/v1/<bridge_id>`. Radiator IDs are the registry IDs in decimal.

| Topic | Direction | Retained | Payload |
| --- | --- | --- | --- |
| `B/availability` | out | yes | `online` or `offline` |
| `B/bridge` | out | no | Bridge snapshot, every 5 s |
| `B/radiators/<id>/state` | out | no | Radiator snapshot, every 5 s |
| `B/passive/<8-hex-address>/state` | out | no | Selected read-only observation, every 5 s while global discovery is enabled |
| `B/radiators/<id>/set` | in | must not be | Command |
| `B/radiators/<id>/result` | out | no | Admission and delivery results |
| `B/errors` | out | no | Admission rejections for an invalid topic |
| `homeassistant/status` | in | – | `online` triggers a discovery and snapshot replay |
| `homeassistant/device/cleverbox_<bridge_id>_passive_<8-hex-address>/config` | out | yes, QoS 1 | Selected passive device discovery |
| `homeassistant/device/cleverbox_<bridge_id>_<id>/config` | out | yes, QoS 1 | [Discovery](home-assistant.md#discovery) |

All other publications are QoS 0.

## Sessions and command IDs

Each broker connection gets a random 32-hex-digit `session`. Every command must
carry the current session, so commands from earlier connections or from before
a reboot are rejected.

Within a session, request IDs come from one bridge-wide counter. A command
identifies itself in one of two ways:

- **Request ID**: `request_id` is an integer from 1 to 2⁵³−1, greater than every
  ID used so far in this session. Several publishers must share one allocator.
  `request_id_floor` in the bridge snapshot shows the current value but is not
  an allocator.
- **Command token**: each radiator snapshot carries a single-use
  `command_token`. Cleverbox allocates the next request ID when the token is
  used. This is what the Home Assistant package uses.

A request ID or token is consumed once the session and ID or token check pass,
even if the command is then rejected (bad fields, busy, invalid relationship). Retry
with a new ID or a fresh token. Wrong sessions and retained messages consume
nothing.

## Commands

Publish to `B/radiators/<id>/set`, not retained. The payload is a flat JSON
object with exactly four fields:

```json
{"session":"<session>","request_id":17,"mode":0,"centi_c":2050}
{"session":"<session>","command_token":"<token>","mode":3,"centi_c":1900}
```

`mode` and `centi_c` follow the [HTTP rules](http-api.md#conventions). Both are
always required. There is no clock override or template field.

## Results

Each command produces an admission result on `B/radiators/<id>/result`, and
accepted commands then produce delivery results there as their phase changes.

```json
{"session":"...","request_id":17,"stage":"admission","accepted":true,"reason":"accepted","error":0}
{"session":"...","request_id":17,"stage":"delivery","phase":"matched","attempts":1,"error":0,"pending":false,"requested_centi_c":2050,"requested_mode":0,"match_age_ms":100}
```

Token commands also echo `command_token`. Delivery results keep the session and
request ID of the original command, even after a reconnect. Phases are listed
in [command delivery](architecture.md#command-delivery).

| Reason | Meaning |
| --- | --- |
| `accepted` | Queued for delivery |
| `invalid_topic` | Topic is not `B/radiators/<id>/set` (published on `B/errors`) |
| `retained_command` | The message had the retain flag |
| `session_mismatch` | Missing or old session |
| `invalid_request_id` | Missing or out-of-range `request_id` |
| `duplicate_or_out_of_order` | `request_id` not above the current counter |
| `invalid_command_token` | Wrong or already used token |
| `request_id_exhausted` | Counter at 2⁵³−1; reconnect for a new session |
| `invalid_fields` | Wrong field set or values |
| `unknown_identity` | No radiator with this ID |
| `listen_only` | RF disabled by operating mode, pending mode transition or mode storage error |
| `time_unavailable` | Device clock not set |
| `invalid_settings_or_relationship` | Settings rejected by the codec |
| `busy_or_unavailable` | Radiator busy, pairing/mutation running or radio not ready |

Only the latest MQTT command per radiator is tracked. Its delivery result is
republished after a reconnect. Results are not stored across reboots.

## Snapshots

Bridge snapshot (`B/bridge`):

```json
{"session":"...","uptime_ms":123456,"request_id_floor":17,"stale_after_ms":300000}
```

Radiator snapshot (`B/radiators/<id>/state`):

```json
{"session":"...","command_token":"...","id":1,"name":"Living room","available":true,"fresh":true,"age_ms":1000,"stale":false,"mode":0,"temperature_centi_c":2010,"target_centi_c":2050}
```

| Field | Meaning |
| --- | --- |
| `available` | A report has been received this boot |
| `age_ms`, `stale`, `mode`, `temperature_centi_c`, `target_centi_c` | Latest report; present only when `available`. Unavailable temperatures are omitted. |
| `fresh` | Report age plus 15 s is below the 5-minute stale threshold |
| `admission` | Latest admission result for this radiator |
| `delivery` | Latest tracked delivery: `session`, `request_id`, `phase`, `attempts`, `error`, requested settings |

Snapshots are not retained. Consumers should treat a snapshot older than 15 s,
or availability `offline`, as unknown.

Passive snapshots use uppercase, zero-padded RF addresses and include
`session`, `radiator`, `name`, `available` and `fresh`. After a report this boot
they add `controller` (the latest destination), `age_ms`, `stale`, `mode` and
available `temperature_centi_c` / `target_centi_c` only. Freshness uses the same
monotonic five-minute threshold and 15-second headroom as paired snapshots.
Selected addresses have independent bounded receive storage, so cache churn
cannot redirect or prevent updates. Reboot publishes unavailable until a new
valid report arrives. There is no passive command topic, token, requested state
or delivery result. Global discovery disable stops passive snapshots.

## Limits

- Incoming payloads up to 512 bytes, topics under 128 bytes, one message being
  reassembled plus 4 queued. Excess or malformed input is dropped and counted in
  `dropped_input`.
- Command JSON must be a single flat object; nested objects, arrays and escaped
  NUL characters are rejected.
- Client buffers: 256 B receive, 1024 B send, 9216 B outbox.

## Broker access

Cleverbox needs to read `B/radiators/+/set` and `homeassistant/status`, and to
write `B/#` and its own `homeassistant/device/cleverbox_<bridge_id>_<id>/config`
topics, including the `cleverbox_<bridge_id>_passive_<address>` namespace. Clients that send commands need to read `B/#` and write
`B/radiators/+/set`. Keep retain disabled for commands. Credentials and
payloads are not encrypted on the network.

## Radio health

The existing `bridge` snapshot adds `radio_health`, `radio_ready`, `control_ready`,
`radio_failure_stage`, `radio_failure_code`, `recovery_attempt`,
`recovery_attempts`, `recovery_successes`, `retry_exhausted` and `retry_in_ms`.
Radio-health transitions trigger a snapshot on the next 100 ms MQTT owner tick;
normal publication remains every five seconds. Bridge transport availability
stays online while its broker session works, including during a radio outage.

Paired and passive state snapshots add `radio_ready` and `radio_health`.
`available` still means a report was received this boot. `fresh` additionally
requires a ready radio. Readings, destination and age remain the last validated
observation; recovery does not refresh them. Exhaustion requires the STA HTTP
retry action, not an MQTT command or automatic reconnect.

Delivery error `0x730A` means uncertain physical delivery. The bridge snapshot
retains the underlying failure stage/code. Tracked command results still name
the original request/session. Reconnect publishes results and observations,
never failed commands. See [the failure policy](architecture.md#radio-health-and-recovery).
