# MQTT

Cleverbox connects to one MQTT 3.1.1 broker over plain TCP. Configure it on the
web page or with [`POST /api/mqtt`](http-api.md#mqtt). MQTT commands use the
same queue as HTTP commands.

## Connection

| Setting | Value |
| --- | --- |
| Client ID | `cleverbox-<bridge_id>` |
| `bridge_id` | Station MAC address, 12 lowercase hex digits |
| Session | Clean session, keepalive 30 s |
| Last will | `offline` on the availability topic, retained, QoS 1 |
| Reconnect | ESP-MQTT reconnects by itself every 5 s |

On each connection Cleverbox subscribes to the command topic and
`homeassistant/status` (QoS 0), publishes retained `online`, then publishes
discovery (if enabled) and snapshots. Disabling MQTT publishes `offline` first.

## Topics

`B` = `cleverbox/v1/<bridge_id>`. Radiator IDs are registry IDs in decimal.

| Topic | Direction | Retained | Payload |
| --- | --- | --- | --- |
| `B/availability` | out | yes | `online` or `offline` |
| `B/bridge` | out | no | Bridge snapshot, every 5 s |
| `B/radiators/<id>/state` | out | no | Radiator snapshot, every 5 s |
| `B/radiators/<id>/set` | in | no | Command |
| `B/radiators/<id>/result` | out | no | Admission result |
| `B/errors` | out | no | Messages on an invalid command topic |
| `homeassistant/status` | in | – | `online` replays discovery and snapshots |
| `homeassistant/device/cleverbox_<bridge_id>_<id>/config` | out | yes, QoS 1 | [Discovery](home-assistant.md#discovery) |

## Commands

Publish to `B/radiators/<id>/set`, not retained:

```json
{"mode":0,"centi_c":2050,"request_id":17}
```

`mode` and `centi_c` follow the [HTTP rules](http-api.md#conventions).
`request_id` is optional (1 to 2⁵³−1); it is echoed in the result and the
snapshot so a client can recognise its command. It is not checked for order or
duplicates.

The admission result on `B/radiators/<id>/result`:

```json
{"request_id":17,"accepted":true,"reason":"accepted"}
```

Rejection reasons are the [HTTP error names](http-api.md#conventions) plus
`invalid_request` (bad JSON or fields) and `retained_command`. A retained
command would be delivered again at every reconnect and boot, so messages that
arrive with the retain flag are rejected. Brokers set that flag only when
delivering a stored message to a new subscription.

Delivery progress appears in the radiator snapshot's `command` object.

## Snapshots

Bridge (`B/bridge`):

```json
{"uptime_ms":123456,"radio_ready":true,"radio_error_stage":"none","radio_error":0,"time_usable":true}
```

Radiator (`B/radiators/<id>/state`):

```json
{"id":1,"name":"Living room","radiator":"F1040506","paired":true,"radio_ready":true,"available":true,"fresh":true,
 "age_ms":1000,"stale":false,"controller":"F1010203","mode":0,"temperature_centi_c":2010,"target_centi_c":2050,
 "command":{"phase":"matched","attempts":1,"error":0,"requested_mode":0,"requested_centi_c":2050,"request_id":17}}
```

| Field | Meaning |
| --- | --- |
| `available` | A report has been received this boot |
| `age_ms`, `stale`, `controller`, `mode`, `temperature_centi_c`, `target_centi_c` | Latest report, when `available`. Unavailable values are omitted. |
| `fresh` | Radio ready and report age plus 15 s below the 5-minute stale threshold |
| `command` | Paired radiators only: latest command, as in the [HTTP radiator object](http-api.md#radiator-object) |

Snapshots are not retained. Treat a snapshot older than 15 s, or availability
`offline`, as unknown.

## Broker access

Cleverbox reads `B/radiators/+/set` and `homeassistant/status`, and writes
`B/#` and `homeassistant/device/cleverbox_<bridge_id>_<id>/config`. Command
clients read `B/#` and write `B/radiators/+/set`. Credentials and payloads are
not encrypted.
