# HTTP API

The device serves the page at `/` and a JSON API under `/api/`. The same server
answers on the setup access point (http://192.168.4.1/) and the station IP.
There is no authentication; station control assumes a trusted LAN. See
[security](architecture.md#security).

## Interface policy

One admission table wraps every API handler before parsing or queuing work.
The page and all listed GET diagnostics are available on AP and STA; passwords
remain redacted. POST admission is:

| Endpoint | AP | STA (trusted LAN) | Unknown |
| --- | --- | --- | --- |
| `/api/network` (configure, forget, finish_recovery) | Allowed | Allowed | Denied |
| `/api/pairing`, `/api/command`, `/api/controller-state`, `/api/radiators` | Denied | Allowed | Denied |
| `/api/time`, `/api/mqtt`, `/api/preview` | Denied | Allowed | Denied |

Denial returns `403` with an explanation and requests socket closure, before
any handler, queued work, persistence change or RF request. The UI disables
unavailable actions and explains how to use the station IP. Configure MQTT
and manual UTC/timezone from the home LAN after Wi-Fi setup. Broker replacement
is excluded from AP recovery because it could redirect command access.

Classification uses the accepted socket's local IPv4 address (`getsockname`)
and current up ESP netif addresses. `Host`, `Origin`, forwarding headers and
client-supplied identities never grant access. Both interfaces can be active
at once. The socket peer (`getpeername`) also guards against an AP client
address targeting the station IP. An overlapping AP/LAN subnet denies those
mutations conservatively. Missing/failed/ambiguous addresses and IPv6 are
unknown; all unknown-interface POSTs fail closed. Read-only diagnostics remain
available. Local-address classification and the peer guard still require
[hardware checks](hardware.md#3-network-and-recovery) of ESP/lwIP routing; this
is neither HTTP authentication nor a network firewall.

## Conventions

- `POST` bodies must be JSON with `Content-Type: application/json` exactly (no
  `charset` parameter). Each endpoint has a body size limit, listed below.
- `202 Accepted` means the request was queued. Poll the matching `GET` for the
  outcome.
- `403` means the receiving interface does not permit the operation.
- `400` means invalid input. `409` means the device cannot act now (busy, not
  ready, missing prerequisite). Both return a plain-text explanation.
- Responses carry `Cache-Control: no-store`.
- Radiators are addressed by numeric `id` (1 to 2³²−1). If `id` is omitted and
  exactly one radiator exists, that radiator is used; with several, the request
  fails with `409`. A malformed `id` is always `400`.
- Addresses are 8 hex digits, for example `F1010203`.
- Temperatures are in hundredths of a degree Celsius (`centi_c`) in requests
  and in degrees Celsius (`temperature_c`, `target_c`) in responses.
- Modes: `0` Comfort, `1` Off, `2` Frost, `3` Eco. Non-Off targets are 5–30 °C.
  Off always sends the raw Off target; pass `centi_c: 0`.

## Endpoints

| Method and path | Body limit | Purpose |
| --- | --- | --- |
| `GET /api/radio[?id=N]` | – | Radio diagnostics, all radiators, one selected radiator |
| `GET /api/radiators` | – | Same response as `/api/radio` |
| `POST /api/radiators` | 255 B | Select, rename, remove |
| `POST /api/pairing` | 255 B | Start, cancel, save pairing |
| `POST /api/controller-state` | 767 B | Save, import, clear template |
| `POST /api/command` | 511 B | Send target and mode |
| `POST /api/preview` | 1023 B | Build a frame without sending it |
| `GET /api/network`, `POST /api/network` | 767 B | Wi-Fi status and setup |
| `GET /api/time`, `POST /api/time` | 255 B | Clock status and setup |
| `GET /api/mqtt`, `POST /api/mqtt` | 767 B | Broker status and setup |

## Radiators

### `GET /api/radio`

Optional query `?id=N` selects a radiator; unknown IDs return `404`.

| Field | Meaning |
| --- | --- |
| `ready`, `error` | CC1101 ready; last RadioLib or ESP-IDF error code |
| `chip_version` | CC1101 version register, `-1` if not read |
| `valid_frames`, `invalid_frames`, `receive_errors` | Counters since boot |
| `capacity` | Maximum radiators in this build |
| `registry_error`, `registry_problem` | Load or migration error: `none`, `load_or_migration_failed`, `stored_registry_exceeds_capacity` |
| `mutation_pending`, `mutation_error` | Latest registry change (rename, remove, template save or clear): queued, and its result code |
| `exchange_id` | ID reserved by a running pairing, else 0 |
| `active_commands` | Radiators with an outstanding command |
| `radiators` | Array of [radiator objects](#radiator-object) |
| `id`, `name`, `pairing`, `command`, `observed` | The selected radiator, if any, at top level |
| `inclusion` | Latest pairing attempt: `id`, `phase`, `saved`, `error`, `duplicate` |
| `last_frame`, `frame_age_ms` | Latest CRC-valid frame from any address (hex), and its age |
| `report` | Latest decoded report from any address: `source`, `destination`, `age_ms`, `temperature_c`, `target_c`, `mode` |

`last_frame` and `report` are global diagnostics, not the selected radiator's
observation.

### Radiator object

| Field | Meaning |
| --- | --- |
| `id`, `name` | Registry ID and label |
| `pairing.controller`, `pairing.radiator` | Saved addresses |
| `command.phase` | `idle`, `poll`, `waiting_report`, `sending`, `waiting_readback`, `matched`, `timeout`, `error` |
| `command.pending` | A command is outstanding |
| `command.attempts`, `command.error` | State frames sent; last error code |
| `command.requested_target_c`, `command.requested_mode` | Requested settings (absent when idle; Off shows 0) |
| `command.match_age_ms` | Time since the matching report (when `matched`) |
| `command.template_source` | `learned`, `saved` or `missing` |
| `command.template_available`, `learned_available`, `saved_available` | Which templates exist |
| `command.template_age_ms` | Age of the learned template |
| `command.save_status`, `command.clear_status` | Latest template save or clear: `not_attempted`, `pending`, `succeeded`, `failed` |
| `command.state_store_error`, `command.state_load_error` | Storage error codes |
| `observed` | Present after the first report this boot: `age_ms`, `stale` (≥ 5 min), `mode`, `temperature_c`, `target_c` (`null` if the radiator reports it unavailable) |

### `POST /api/radiators`

| Request | Effect | Success |
| --- | --- | --- |
| `{"action":"select","id":1}` | Return the radiator object. Changes nothing on the device. | `200` |
| `{"action":"rename","id":1,"name":"Living room"}` | Queue a rename | `202` |
| `{"action":"remove","id":1}` | Queue removal of the record; `409` while a command is outstanding | `202` |

`202` responses are `{"accepted":true,"transmitted":false}`. Check
`mutation_pending` and `mutation_error` for the result.

### `POST /api/pairing`

| Request | Effect |
| --- | --- |
| `{"action":"start","source":"F1010203","destination":"F1100001"}` | Start pairing. `source` is the controller address Cleverbox will use; `destination` is the discovery address. Returns `{"accepted":true,"id":N}` with the ID the new record will get. |
| `{"action":"cancel","id":N}` | Stop the remaining steps |
| `{"action":"save","id":N}` | Retry saving after a completed pairing whose save failed; sends nothing |

Start returns `409` while another pairing, any command or a registry change is
outstanding, at capacity, or if the radio is not ready. Track progress in
`inclusion`. See [pairing](architecture.md#pairing).

### `POST /api/controller-state`

| Request | Effect |
| --- | --- |
| `{"action":"save","id":1}` | Save the learned template; `409` if none |
| `{"action":"import","id":1,"state":"<118 hex digits>"}` | Validate and save a template; destination must be the radiator's address |
| `{"action":"clear","id":1}` | Remove saved and learned templates |

Returns `{"accepted":true,"transmitted":false}`. Track `command.save_status`
and `command.clear_status`.

### `POST /api/command`

```json
{"id":1,"mode":0,"centi_c":2050}
```

| Field | Required | Meaning |
| --- | --- | --- |
| `id` | yes (see conventions) | Radiator |
| `mode`, `centi_c` | yes | Requested settings |
| `state` | no | Template for this command only (hex). Invalid values fail; they never fall back to another template. |
| `clock_seconds` | forbidden on physical firmware | Presence returns `400`, including null or malformed values. Only host builds accept 0–86399 for tests. |

Returns `{"status":"queued"}` with `202`. `409` cases: clock not set, radiator
busy, pairing or registry change running, radio not ready, no template.
Track `command.phase`; see [command delivery](architecture.md#command-delivery).

### `POST /api/preview`

Builds a frame and returns it without transmitting:
`{"status":"preview","transmitted":false,"frame":"<hex>"}`.

```json
{"command":"offer","source":"F1010203","destination":"F1100001"}
{"command":"target","state":"<template hex>","mode":0,"centi_c":2050}
```

`command` is `offer`, `bind`, `poll` or `target`. `target` sets bytes 35–37 and
both CRCs and keeps everything else, including the clock.

## Network

### `GET /api/network`

| Field | Values |
| --- | --- |
| `state` | `disconnected`, `connecting`, `connected`, `failed` |
| `setup_result` | Latest configure: `none`, `pending`, `saved`, `failed` |
| `configured` | Saved credentials exist (with `persistence_uncertain`: only a RAM copy is known) |
| `ip` | Station IPv4 address, empty if none |
| `attempts` | Attempts in the current batch |
| `error` | `none`, `connection_failed`, `storage_failed`, `persistence_uncertain`, `storage_invalid`, `platform_failed` |
| `persistence_uncertain` | The saved Wi-Fi record after reboot is unknown |
| `recovery_ap` | Access point currently on |
| `ap_closing` | A 60 s closure grace period is active, or a failed close is being retried |
| `http_interface`, `http_control_allowed` | This request classified as `ap`, `sta` or `unknown`; full control only on STA |
| `manual_recovery`, `recovery_error` | Button latch active; `none` or `ap_open_failed` |
| `event_queue_overflows` | Lost Wi-Fi events (saturating) |

SSID and password are never returned.

### `POST /api/network`

| Request | Effect |
| --- | --- |
| `{"action":"configure","ssid":"...","password":"..."}` | Try new credentials; save after DHCP succeeds |
| `{"action":"finish_recovery"}` | Close AP after 60 s, even without STA or certain storage; current AP users lose access |
| `{"action":"forget","confirm":true}` | Erase saved Wi-Fi only |

Requests must contain exactly these fields. Returns `{"accepted":true}` with
`202`, or `409` while another action is pending or (for `configure`) a
connection attempt is running. See [network](architecture.md#network).

## Time

### `GET /api/time`

| Field | Meaning |
| --- | --- |
| `usable` | Commands can be admitted |
| `source` | `unsynchronized`, `manual`, `network` |
| `unix_seconds`, `utc`, `local` | Current time (only when usable) |
| `timezone` | Active POSIX rule |
| `synchronized_this_boot`, `sync_age_ms` | Whether SNTP has synced since boot, and how long ago (`null` before) |
| `network_connected` | Station has an IP; not proof that SNTP works |
| `sntp_error`, `config_error`, `persistence_uncertain` | Error codes; time zone save unconfirmed |

### `POST /api/time`

| Request | Effect |
| --- | --- |
| `{"action":"timezone","timezone":"CET-1CEST,M3.5.0/2,M10.5.0/3"}` | Validate and save the rule |
| `{"action":"manual","unix_seconds":1767268800}` | Set UTC until reboot or next SNTP sync (2020–2099) |

Returns `{"updated":true,"transmitted":false}` with `200`, `400` for invalid
input, `409` if saving failed.

## MQTT

### `GET /api/mqtt`

| Field | Meaning |
| --- | --- |
| `configured`, `enabled`, `host`, `port`, `username` | Saved settings (password never returned) |
| `pending` | A configuration action is queued |
| `broker_connected` | Broker accepted the connection |
| `subscription_pending`, `subscription_error` | Waiting for the command subscription; its error |
| `connected` | Command subscription confirmed; commands accepted |
| `bridge_id`, `session` | Topic identity; current session (empty when not connected) |
| `config_error`, `client_error`, `persistence_uncertain` | Error codes; save unconfirmed |
| `discovery_enabled`, `discovery_pending`, `discovery_error` | Home Assistant discovery state |
| `dropped_input` | Discarded incoming MQTT messages |

### `POST /api/mqtt`

```json
{"action":"configure","host":"192.168.1.10","port":1883,"username":"","password":"","enabled":true}
```

All six fields are required; each configure replaces the stored record. Host
is a hostname or IPv4 address (letters, digits, dots, hyphens; up to 96
characters). Username up to 64 and password up to 128 printable ASCII
characters; a password requires a username.

Single-field actions: `{"action":"enable"}`, `disable`, `forget`,
`discovery_enable`, `discovery_disable`.

Returns `{"accepted":true,"transmitted":false}` with `202`. `409` while another
action is pending, or when changing broker host/port or forgetting while
discovery records remain (disable discovery and wait for
`discovery_pending: 0` first).

## Simulation only

Host builds alone accept the command clock override and add `POST /api/simulation`. The physical firmware has no such
endpoint.

| Action | Fields | Effect |
| --- | --- | --- |
| `device` | `index` 0 or 1 | Choose which invented radiator answers the next inclusion and fixture |
| `fixture` | – | Inject a synthetic controller-state frame for the chosen radiator |
| `scenario` | `name`: `normal`, `delayed`, `silence`, `silence_a`, `ack_only` | Reply timing: 300 ms, 2.5 s, none, none from radiator A, short acks only |

With `serve.py --test-clock` the clock is frozen and more actions exist:
`advance` (`ms`), `inject` (`frame` hex), `time_sync` (`unix_seconds`),
`network_event` (`name`: `started`, `disconnected`, `lost_ip`, `got_ip`), `button_level`
(`low`), `wifi_storage_fault` (`mode` 0–2), and MQTT test hooks
(`mqtt_connection`, `mqtt_suback`, `mqtt_input` and others; see
`host/serve.py`). `GET /api/simulation/mqtt` drains published MQTT messages.

For policy tests, `--test-interfaces` adds loopback listeners on 127.0.0.2
(AP) and 127.0.0.3 (unknown) beside 127.0.0.1 (STA), all sharing one application.
These are netif fixtures, not physical interfaces. With `--test-clock`,
`GET /api/simulation/radio` supplies host-only RF observations, including when
testing the physical HTTP compilation branch. No such endpoint exists on ESP.
