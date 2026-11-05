# HTTP API

The device serves the page at `/` and a JSON API under `/api/`, on the setup
access point (http://192.168.4.1/) and on the station IP. There is no
authentication: anyone who can reach the device has full control.

## Conventions

- `POST` bodies are JSON with `Content-Type: application/json`, under 1 KiB.
- Settings changes are saved before the response: `200` means done.
  `202 Accepted` means radio work was queued; poll `GET /api/radio` for the outcome.
- Errors return `{"error":"<name>","code":N}`: `400` invalid request, `404`
  unknown radiator, `409` not possible now, `500` storage error.
- Error names: `radio_not_ready`, `busy`, `time_unavailable`, `registry_full`,
  `already_registered`, `not_paired`, `invalid_request`, `unknown_radiator`,
  `error` (see `code`).
- Radiators are addressed by numeric `id`. RF addresses are 8 hex digits, for
  example `F1010203`.
- Requests use hundredths of a degree (`centi_c`); responses use degrees
  (`temperature_c`, `target_c`).
- Modes: `0` Comfort, `1` Off, `2` Frost, `3` Eco. Non-Off targets are 5–30 °C.
  Off sends the raw Off target; pass `centi_c: 0`.

## Endpoints

| Method and path | Purpose |
| --- | --- |
| `GET /api/radio` | Radio status, radiators, pairing, heard radiators |
| `POST /api/radio` | `{"action":"retry"}`: set up a radio that is not ready |
| `POST /api/pairing` | Start or cancel pairing |
| `POST /api/command` | Send a target and mode |
| `POST /api/radiators` | Rename, remove, monitor |
| `POST /api/preview` | Build an offer, bind or poll frame without sending it |
| `GET`/`POST /api/network` | Wi-Fi |
| `GET`/`POST /api/time` | Device time |
| `GET`/`POST /api/mqtt` | MQTT broker and Home Assistant discovery |

## `GET /api/radio`

| Field | Meaning |
| --- | --- |
| `ready` | CC1101 set up and receiving |
| `error`, `error_stage` | Last radio error (RadioLib code) and stage: `none`, `init`, `receive`, `transmit`. Kept after a successful retry. |
| `chip_version` | CC1101 version register, `-1` if not read |
| `valid_frames`, `invalid_frames`, `receive_errors`, `transmissions` | Counters since boot |
| `capacity`, `registry_error` | Maximum radiators; last registry load or save error |
| `last_frame`, `frame_age_ms` | Latest CRC-valid frame from any address (hex) |
| `pairing` | Latest attempt: `id`, `phase`, `error`, `error_name`, `controller`, `discovery`, `radiator` |
| `radiators` | [Radiator objects](#radiator-object) |
| `observations` | Latest report per heard radiator address: `radiator`, `controller` (the report's destination), `age_ms`, `stale`, `mode`, `temperature_c`, `target_c`, and `id` if registered |

`ready: false` with an error means the radio stopped; see
[radio errors](architecture.md#radio-errors). `POST /api/radio` with
`{"action":"retry"}` returns `202`, or `409` while the radio is ready.

### Radiator object

| Field | Meaning |
| --- | --- |
| `id`, `name`, `radiator` | Registry ID, label and RF address |
| `paired`, `controller` | Paired (Cleverbox is its controller) or monitored only |
| `observed` | After a report this boot: `age_ms`, `stale` (≥ 5 min), `controller`, `mode`, `temperature_c`, `target_c` (`null` if unavailable) |
| `command` | Paired only: `phase` (see [commands](architecture.md#commands)), `attempts`, `error`, `error_name`, and once requested `requested_mode`, `requested_target_c`; `match_age_ms` when matched |

## `POST /api/pairing`

| Request | Effect |
| --- | --- |
| `{"action":"start","source":"F1010203","destination":"F1100001"}` | Start pairing. `source` is the controller address Cleverbox will use, `destination` the discovery address; same `F1` or `D0` family. Returns `202` with `{"id":N}`, the new record's ID. |
| `{"action":"cancel"}` | Stop the remaining steps |

Start returns `409` while the radio is not ready, a pairing or command is
outstanding (`busy`) or the registry is full. Track progress in `pairing`.

## `POST /api/command`

```json
{"id":1,"mode":0,"centi_c":2050}
```

Returns `202` when queued. `409` cases: `not_paired`, `radio_not_ready`,
`busy` (pairing running or this radiator already has a command),
`time_unavailable`.

## `POST /api/radiators`

| Request | Effect |
| --- | --- |
| `{"action":"rename","id":1,"name":"Living room"}` | Rename |
| `{"action":"remove","id":1}` | Remove the record (`409` while it has a command). The radiator itself stays paired to this controller address. |
| `{"action":"monitor","radiator":"F1445566","name":"Hall"}` | Add a monitored record; nothing is sent |

All return `200` with `{"id":N}`. Names are 1–32 bytes without control characters.

## `POST /api/preview`

`{"command":"offer","source":"F1010203","destination":"F1100001"}` returns
`{"frame":"<hex>"}`. `command` is `offer`, `bind` or `poll`. Nothing is sent.

## Network

`GET /api/network` returns `state` (`disconnected`, `connecting`, `connected`),
`ip`, `configured`, `ap` (setup AP open) and `error`. The password is never returned.

| Request | Effect |
| --- | --- |
| `{"action":"configure","ssid":"...","password":"..."}` | Save and connect (SSID 1–32 bytes; password 8–63 printable ASCII) |
| `{"action":"finish_recovery"}` | Close the setup AP; `409` without saved Wi-Fi |
| `{"action":"forget"}` | Erase Wi-Fi and open the setup AP |

## Time

`GET /api/time` returns `usable`, `source` (`unset`, `manual`, `network`),
`timezone`, and when usable `unix_seconds`, `utc` and `local`.

| Request | Effect |
| --- | --- |
| `{"action":"timezone","timezone":"CET-1CEST,M3.5.0/2,M10.5.0/3"}` | Save the POSIX rule |
| `{"action":"manual","unix_seconds":1767268800}` | Set UTC until restart or the next SNTP sync (2020–2099) |

## MQTT

`GET /api/mqtt` returns `configured`, `enabled`, `connected`,
`discovery_enabled`, `bridge_id`, `host`, `port`, `username` and `error`. The
password is never returned.

```json
{"action":"configure","host":"192.168.1.10","port":1883,"username":"","password":"","enabled":true}
```

Host is a hostname or IPv4 address. Username up to 64 and password up to 128
printable ASCII characters. Other actions: `{"action":"enable"}`, `disable`,
`forget`, `discovery_enable`, `discovery_disable`. See [MQTT](mqtt.md).

## Simulation only

The host simulator adds `POST /api/simulation` and simulation fields
(`simulation`, `scenario`, `simulation_time_ms`, `simulation_tx`) in
`GET /api/radio`. The firmware has neither.

| Action | Fields | Effect |
| --- | --- | --- |
| `device` | `index` 0 or 1 | Which invented radiator answers the next offer |
| `scenario` | `name` | `normal` (300 ms replies), `delayed` (2.5 s), `silence`, `silence_a` (first radiator silent), `ack_only` |

With `serve.py --test-clock` the clock is frozen and these also work:
`advance` (`ms`), `inject` (`frame` hex, delivered as received),
`radio_fault` (`stage` `init`/`receive`/`transmit`, `code`; 0 clears),
`time_sync` (`unix_seconds`), `network_event` (`got_ip`, `disconnected`),
`button` (`low`), `mqtt_connection` (`connected`), `mqtt_message` (`topic`,
`payload`, `retained`). `GET /api/simulation/mqtt` returns and clears the fake
client's publications.
