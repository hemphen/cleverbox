# MQTT transport (v1)

MQTT transport, opt-in [Home Assistant discovery and draft/Apply interaction](home-assistant.md)
are implemented. HTTP remains available. No scheduler, history, Matter or OTA is included. MQTT does not own or duplicate radio delivery machines.

## Setup and persistence

Use **MQTT broker** on the HTTP page, or `POST /api/mqtt`:

```json
{"action":"configure","host":"192.168.1.10","port":1883,"username":"","password":"","enabled":true}
```

All six fields are required. Each configure replaces the entire record; enter
credentials again when editing. Hostnames/IPv4 use 1–96 ASCII letters, digits,
dots or hyphens; no URI, embedded credentials, IPv6 or path. Port is 1–65535.
Username (0–64) and password (0–128) are printable ASCII; a password requires a
username. Empty credentials select anonymous access. `enabled` is a JSON boolean.
The initial state is unconfigured/disabled. `enable`, `disable` and `forget` are
single-field action requests. Enable/disable retain existing credentials. Forget
writes an empty disabled record and preserves radiator, Wi-Fi and time data.
None of these operations cancels an admitted RF command or changes radiator settings.

202 means queued, not saved; 400 means invalid, 409 means a configuration action
is pending. Poll `GET /api/mqtt`: `configured`, `enabled`, `connected`, `pending`,
`bridge_id`, `session`, `host`, `port`, `username`, `config_error`, `client_error`,
`persistence_uncertain`, `dropped_input`, `broker_connected`, `subscription_pending`,
`subscription_error`. Passwords are never returned. Status and
actions are no-store. A failed save retains the previous active RAM settings and
sets persistence uncertainty: the next boot may load old/new/missing/invalid
configuration. There is no rollback or power-loss guarantee. Explicit retry
forces erase/commit, rewrite/commit and fresh-handle verification, including for
same-value retries and the forget tombstone. Success clears uncertainty. Invalid
boot configuration is reported and leaves MQTT disabled; no automatic erase.

`broker_connected` means a broker CONNACK was received; `connected` means command
access is ready after the matching successful SUBACK. While awaiting SUBACK,
availability stays `offline`, the session is empty and no commands are admitted
or observations/results published. Only the exact subscription packet ID with
one QoS-0 grant succeeds. Rejection/malformed grants report `subscription_error`
29697 (`0x7401`), a missing/late acknowledgement after **five monotonic seconds**
reports 29698 (`0x7402`), and a failed subscribe request reports 29699 (`0x7403`).
The client is closed (clearing pending input/outbox) and recreated no sooner than
**five seconds** later. Wrong/late ACK IDs cannot establish readiness; disconnect
invalidates the outstanding ACK. The last subscription error stays visible during
retries and clears only after a successful SUBACK. RF already admitted continues
independently throughout. The UI distinguishes waiting for command subscription
from readiness and displays rejection/timeout explicitly.

Only `cleverbox/mqtt` is written: fixed version-1 layout (295 bytes): version,
flags (configured bit 0, enabled bit 1), LE16 port, then zero-padded NUL-terminated
host (97), username (65), password (129). No commands, sessions or observations
are persisted. NVS encryption and HTTP login/HTTPS are not enabled. This milestone
supports **TCP on a trusted LAN**, without TLS; credentials cross the LAN in
cleartext. Restrict broker accounts with topic ACLs and protect local HTTP/AP
access. Client debug/payload logging is disabled; do not commit credentials or
household capture/configuration files.

## Topics and payloads

Prefix `B = cleverbox/v1/<bridge_id>`; `bridge_id` is the full station base MAC
as 12 lowercase hex digits (stable across firmware/configuration restarts).
Client ID is `cleverbox-<bridge_id>`. Radiator IDs are the existing stable,
nonzero registry uint32 IDs, printed in canonical decimal. Names are metadata,
never routing inputs. Payloads are JSON except plain availability strings.

| Topic | Direction | Payload / retention |
| --- | --- | --- |
| `B/availability` | bridge → broker | `online` / `offline`, retained; retained `offline` last will |
| `B/bridge` | bridge → clients | `session`, `uptime_ms`, `request_id_floor`, `stale_after_ms`; nonretained |
| `B/radiators/<id>/set` | clients → bridge | complete immediate command below; never retain |
| `B/radiators/<id>/result` | bridge → clients | separate admission and delivery events below; nonretained |
| `B/radiators/<id>/state` | bridge → clients | observation snapshot below; nonretained |
| `B/errors` | bridge → clients | admission rejection when no valid topic identity exists; nonretained |

The original numeric command form requires exactly these four fields:

```json
{"session":"<current 32-character session>","request_id":1,"mode":0,"centi_c":2050}
```

Read the current session from `B/bridge` (or HTTP). Every broker connection gets
an independent random 128-bit session, including after restart. The request ID
must be an integer 1–9007199254740991, **strictly greater** than every request ID
already consumed this session. This is a single bridge-wide high-water mark;
multiple command producers must coordinate IDs (e.g. one controller/allocator),
not maintain independent counters. Rejections after a valid session/new ID also
consume the ID; retries use a new ID. The published floor refreshes every five
seconds and is advisory, not a multi-writer allocation API. This bounded rule
needs no evicting dedup cache and applies for the entire connection lifetime.

Modes and target validation are identical to HTTP: 0 Comfort, 1 Off, 2 Frost,
3 Eco. Always supply both fields, including Off (`centi_c:0` convention). Non-Off
uses the existing 5–30 °C limits and codec quantization. No missing target/mode
is filled from an observation or a default. The MQTT surface deliberately has no
clock override or template import: configure time and learn/save/import templates
through HTTP first. Admission uses `radio_control_start_device_tracked()` which
shares HTTP's device-time snapshot, template selection, validation, per-radiator
busy reservation and bounded report-driven delivery. Pairing stays exclusive.

Admission example:

```json
{"session":"...","request_id":1,"stage":"admission","accepted":true,"reason":"accepted","error":0}
```

Rejections use `accepted:false` and a fixed reason: `invalid_topic`,
`retained_command`, `session_mismatch`, `invalid_request_id`,
`duplicate_or_out_of_order`, `invalid_fields`, `unknown_identity`,
`time_unavailable`, `invalid_settings_or_template`, or
`busy_or_unavailable_or_missing_template`. The token form adds the reasons
documented below. An unparseable ID has no request ID in
the response. The response session is the current connection, including when
rejecting an old session. The ID is already explicit in the topic.

Subsequent delivery publications use the **original admission session/ID**:

```json
{"session":"...","request_id":1,"stage":"delivery","phase":"matched","attempts":1,"error":0,"pending":false,"requested_centi_c":2050,"requested_mode":0,"match_age_ms":100}
```

Phases use the existing radio phase names. `matched` means a subsequent matching
radiator report, not proof of causality or heating output. Timeout/error may be
reconciled by a late report without any new TX. A bounded per-radiator correlation
mailbox preserves the tracked result if a later HTTP command runs before the
MQTT task reads it. Only the latest MQTT command per radiator is tracked; there
is no result history. On reconnect that latest result is republished with its
original correlation, including when RF completed during the outage. Admission
responses and intermediate transitions are best effort; they are not backlogged.
HTTP-originated commands remain visible through HTTP and reported observations.

State example:

```json
{"session":"...","id":1,"name":"Living room","available":true,"age_ms":1000,"stale":false,"mode":0,"temperature_centi_c":2010,"target_centi_c":2050}
```

Only actual received observations supply mode/temperatures. Unavailable
measurements are omitted. Before reception this boot, state has `available:false`
and no age, stale flag, mode or temperatures; restored pairing/templates are
configuration, never fresh observations. The five-minute stale threshold is the
existing UI policy, not a protocol heartbeat. Snapshots refresh every five
seconds and on reconnect. Age uses monotonic time, independent of clock edits.
Consumers must also age the snapshot locally and expire it after **15 seconds**
without an update, and treat bridge offline as transport unavailable. Never rely
on an old `stale:false` indefinitely. Snapshots also supply conservative `fresh` (15-second headroom), the latest
`admission` object when present and separate `delivery` details for discovered
diagnostics. These are outcomes, never observations; they do not alter the command
contract. State is nonretained to avoid frozen ages
being delivered as fresh to a later subscriber. Availability is independent of
per-radiator freshness. Removed IDs stop publishing; clients should expire their
snapshots, not infer that a device is still present from historical messages.

## Home Assistant single-use command tokens

Each radiator snapshot additionally supplies a random 32-character hex
`command_token`. Clients may replace `request_id` with that token, keeping a
complete four-field command on the same `/set` topic:

```json
{"session":"<current session>","command_token":"<radiator snapshot token>","mode":3,"centi_c":1900}
```

The MQTT owner validates the current session, identity and exact current token,
then atomically allocates `high_water + 1` from the **same** bridge-wide sequence
used by numeric clients. It consumes/rotates the token **before** field validation
and RF admission, including busy, missing time/template or invalid settings
rejections. The next snapshot is scheduled immediately. Token consumption never
queues RF by itself. A repeated/wrong/previously consumed token returns
`invalid_command_token`; a token plus `request_id`, malformed token or invalid
schema returns `invalid_fields`. Wrong sessions and retained messages do not
consume the current token. At the maximum safe request ID, token admission fails
closed with `request_id_exhausted` until a new connection session; no wrap occurs.

Admission and delivery results echo `command_token` as well as the allocated
numeric ID, so a client can correlate without preallocating a counter. An invalid
or duplicate token has no allocated request ID. Existing numeric payloads and
results remain compatible; every consumed numeric command for a known radiator
also rotates that radiator's token. Numeric producers must still coordinate their
own IDs, and may see rejection if another producer advanced the common sequence.
The published `request_id_floor` is never an atomic allocator.

There is one permit per configured identity in bounded static memory, no evicting
dedup cache, flash writes, command persistence or external allocator. Unconsumed
tokens remain stable between snapshots; all are replaced on a connection change,
and removal discards that identity's token. Re-adding uses a new stable ID.
Reports do not reset the token. It identifies an explicit complete Apply, not a
compare-and-swap of reported settings or a partial setting patch. The HA package
uses independent draft inputs and never fills a missing field from observations.
If two producers or rapid actions use one token, only one can consume it. Inspect
the rejection and explicitly Apply the intended complete draft again after a
new snapshot; do not automatically retry old drafts.

## Session, delivery and resource policy

MQTT 3.1.1, clean session, subscription QoS 0, command/result/state publications QoS 0. Discovery configurations/cleanup use
retained QoS 1 with one acknowledged publication in flight; see [lifecycle](home-assistant.md#discovery-lifecycle-and-persistence).
Last will is retained QoS 1, with 30-second keepalive. Command delivery is best
effort: no offline command backlog, QoS retransmission queue or exactly-once
claim. Never retain commands. Deliveries with the MQTT RETAIN flag are rejected.
MQTT 3.1.1 clears RETAIN when forwarding a live publication to existing
subscribers, so that live delivery cannot be distinguished from another immediate
command. Its retained copy is rejected on resubscription, and the session check
also makes old commands unusable. Publishers must obey this contract.

Repeated or lower request IDs never readmit RF, regardless of MQTT packet IDs or
DUP handling. Disconnect clears incomplete input and queued input. New sessions
reject old payloads, even following a device reboot; sessions/dedup markers are
not restored. Reconnect starts no pairing, polls or target changes. An already
admitted RF command runs without broker/Wi-Fi availability. Publish/subscribe and
client lifecycle calls run on a separate task, never the RF worker or MQTT event
callback. Graceful stop attempts retained offline before DISCONNECT; failed
publication leaves the will armed by closing the transport without DISCONNECT.

Input is at most 512 bytes, topic below 128 bytes, one partial message and four
complete queued messages. Contiguous offset/total checks assemble ESP-MQTT
fragments (later callbacks may omit the topic); malformed/oversized/NUL input is
discarded and counted. A full queue drops new input, with no RF. Flat JSON limits
parser recursion and rejects duplicate/missing/extra fields by exact schema.
The owner ticks every 100 ms; output is bounded per radiator, with snapshots at
five-second intervals. Capacity-dependent tracking lives in static memory.
Client network calls have a one-second timeout, 256-byte RX/1024-byte TX buffers,
9216-byte IDF outbox limit (one discovery config plus subscription control traffic), and paced
five-second reconnect. No socket operation blocks the RF worker. NVS writes are
explicit configuration actions only.

ESP-MQTT is the supported built-in component from **ESP-IDF v5.5.1**, whose
submodule is pinned to `6af4446a48ea7fa54948edc4e62277ed70abb6e1` (no new external
firmware library). The component manifest requires exactly IDF 5.5.1 and CI uses
`espressif/idf:v5.5.1`. Consulted official
[API/configuration/event documentation](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32c6/api-reference/protocols/mqtt.html)
and [pinned client header](https://github.com/espressif/esp-mqtt/blob/6af4446a48ea7fa54948edc4e62277ed70abb6e1/include/mqtt_client.h).

## Validation and board acceptance

`make test test-http test-capacity` includes the production HTTP/UI, NVS,
MQTT routing/publication, device-time admission and RF worker against a small
fake MQTT client boundary. These tests are not a real broker test. Test scenarios
cover matching/rejected/missing/late SUBACKs, paced retry, validation,
busy/independent delivery, retained and duplicate input,
fragmentation, lost links, restart without replay, unavailable boot observations,
requested/observed separation and staleness. See the PR validation results for
the separate local-broker smoke test and ESP32-C6 build.

`make test-mqtt-broker` additionally needs Mosquitto 2.x on PATH and
`python3 -m pip install paho-mqtt==2.1.0`. It starts a temporary loopback-only
broker with anonymous access, real Paho MQTT 3.1.1 sockets and the production C
MQTT service/HTTP/time/RF simulator. This test covers actual topics/JSON,
admission/readback, duplicates, retained resubscription, graceful offline,
abrupt-close last will, broker process restart and RF completion while offline.
The Paho adapter has no routing/reconnect policy. The production MQTT owner
recreates both socket adapters after disconnect at a five-second pace, destroying
any unacknowledged QoS-1 outbox before publishing newer discovery state. ESP-MQTT
automatic reconnect is disabled; clean sessions alone do not remove that outbox.
The optional Python dependency is pinned in the test instructions and is never
imported into the physical firmware or ordinary simulator.

The MQTT firmware uses IDF's standard 1500 KiB single factory-app partition table
because the default 1 MiB app slot was too small. Existing NVS/PHY/app offsets
are preserved; see [firmware build notes](../firmware/README.md).

The separate `make test-home-assistant` runs the pinned actual HA Core against
a broker and this production simulator; see [setup and validation](home-assistant.md).
It is distinct from these fake-client and broker-only tests.

Physical acceptance remains required for broker authentication/DNS over actual
Wi-Fi, AP/STA recovery while MQTT reconnects, keepalive/last will on power or
router loss, task/heap high-water marks, NVS power-cycle retention, actual RF
readback and continued heating without Touch E3/HA. Software and broker smoke
tests do not establish these properties.
