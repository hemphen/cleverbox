# Radiator registry and serialized delivery

The registry is implemented; independent per-radiator delivery is the **next**
milestone. One pairing or command exchange owns the radio globally. Another
exchange receives HTTP 409 immediately, including while the admitted action is
still queued or being handed to the worker. There is no command backlog.

Every configured radiator owns its identity, name, pairing, saved state, learned
state, observations and latest command result. Completing B does not erase A's
result. Reports and controller frames are routed by both source and destination;
selection is never a routing input. One active command machine owns the admitted
frame snapshot and fresh-report/readback exchange. After timeout/TX failure,
matching late reports can reconcile the per-device result without another send.
No independent waiting exchanges or scheduler are implemented.

## HTTP identities

`GET /api/radiators` (also `GET /api/radio`) lists `radiators`, `capacity`,
`registry_error`, `registry_problem`, `exchange_id`, `mutation_pending` and
`mutation_error`, alongside global RF diagnostics. Each radiator contains `id`,
`name`, `pairing`, `command` and, only after reception, `observed`.
`inclusion` describes the latest inclusion attempt, with its prospective `id`,
`phase`, `saved`, `error` and `duplicate` flag.

`GET /api/radio?id=1` returns that radiator's state at the top level as well as
the list. Unknown IDs return 404; malformed IDs return 400. The top-level
`report` and `last_frame` are explicitly global diagnostics, not selected-device
observations. Only `observed` belongs to the selected radiator.

Use JSON numeric `id` on every mutation/command:

| Endpoint | Example / behavior |
| --- | --- |
| `POST /api/radiators` | `{"action":"select","id":1}` returns the device, HTTP 200; no server state changes. |
| `POST /api/radiators` | `{"action":"rename","id":1,"name":"Living room"}` queues a local save. |
| `POST /api/radiators` | `{"action":"remove","id":1}` queues deletion of local configuration only. |
| `POST /api/pairing` | `{"action":"start","source":"F1010203","destination":"F1100001"}` reserves a new identity; HTTP 202 returns `id`. |
| `POST /api/pairing` | `{"action":"cancel","id":2}` or `{"action":"save","id":2}` addresses that inclusion attempt; save explicitly retries failed persistence. |
| `POST /api/controller-state` | `{"action":"save","id":1}`, `{"action":"import","id":1,"state":"<hex>"}` or `{"action":"clear","id":1}`. |
| `POST /api/command` | `{"id":1,"mode":0,"centi_c":2050,"clock_seconds":43200}`; optional per-command `state` retains import precedence. |

IDs are nonzero uint32 values, monotonically assigned to committed records and
never renumbered or reused after removal. Names contain 1–32 UTF-8 bytes, without
ASCII control characters. They are rendered as text in the browser. Name length
is a byte limit, so non-ASCII names may reach it before 32 characters.

Missing IDs retain legacy behavior only when exactly one radiator is configured.
Multiple records make omitted identities ambiguous (409), regardless of any
browser selection. Explicit malformed IDs never fall back. Pair start creates
its identity; cancel/save without ID are legacy-compatible only with at most one
configured radiator. Previews remain standalone non-transmitting frame utilities.

202 means admitted, not committed or applied. Poll `mutation_pending` /
`mutation_error` for rename/remove, per-device save/clear status for state storage,
`inclusion` for pairing and per-device `command` for delivery. Each has bounded
pending work: one shared worker queue slot and one local-storage reservation.
An active record cannot be removed, including before the worker dequeues it.
Local storage changes to other records may run between radio ticks. A failed
configuration save leaves the previous RAM configuration available.

Pairing at capacity fails with 409. An inclusion reply from an already configured
radiator sets `inclusion.duplicate` and fails before bind transmission; no record
is overwritten. Removing a record does **not** reset/unpair the physical radiator.
The browser clears manual template input and old observation displays when
selection changes. Every later request carries its captured identity; another
client's selection has no effect.

## Persistence and migration

One NVS key, `cleverbox/registry`, uses an explicit version-1 byte layout:

- Header: version (1 byte), count (LE16), next identity (LE32).
- Each record: ID (LE32), NUL-terminated name (33 bytes), controller and radiator
  addresses (4 bytes each), saved-state flag (1 byte), exact state (59 bytes).

The serialized size is `7 + 105 * count` (847 bytes at capacity eight), independent
of C padding. Inactive state bytes are zero. Only explicitly saved/imported state
is persisted. Learned state, observations, save results and pending commands are
transient. Boot loads configuration, starts reception and sends nothing.

If the registry key is absent, boot reads the old nine-byte version-1 `pairing`
and optional 68-byte `control_state` (or nine-byte clear tombstone). It validates
addresses, state CRCs, layout and destination, assigns ID 1 and name `Radiator 1`,
and commits the registry. Both legacy keys remain unchanged. If migration fails,
the error is visible and the legacy setup is retained for retry on restart.
An invalid optional legacy state exposes the usable pair with `state_load_error`;
explicit import/save/clear can repair and commit the registry. Other configuration
mutations remain blocked until repair. Unknown/corrupt pairing is never reset.

A present registry, including a committed empty registry, is authoritative.
Migration never runs over it. Consequently removing the migrated radiator and
restarting cannot resurrect it from the legacy keys. Registry version, size,
identities, duplicate addresses, names and saved-state CRCs/layouts are checked
on load. Unknown or corrupt registries block mutation and retain their bytes.
There is no downgrade conversion to the single-radiator firmware; retained legacy
keys represent the old setup, not subsequent registry changes.

Writes build a prospective wire record without publishing changes to RAM. The
worker commits it, then publishes the change. Commit failure restores previous
bytes, or erases the newly created registry key if it was previously absent.
This absence restoration is essential for retrying migration. If restoration
also fails, the invalid-state error means disk recovery cannot be guaranteed
under continued storage failure. Hardware power-loss durability remains pending.

## Capacity and memory

`CLEVERBOX_RADIATOR_CAPACITY` in `device_store.h` defaults to eight. Override it
consistently for all components, e.g. host
`make BUILD_DIR=build/three CPPFLAGS=-DCLEVERBOX_RADIATOR_CAPACITY=3 test test-http`;
for ESP-IDF pass the same definition in both C and C++ compilation flags. It is
not a protocol device limit. Capacity changes do not alter the wire format.

A stored registry that exceeds this build's capacity fails visibly with
`registry_problem: "stored_registry_exceeds_capacity"` and
`registry_error: DEVICE_STORE_ERR_CAPACITY` (31745). It exposes no partial list,
blocks mutations/inclusion and never truncates or overwrites the stored record.
Restore a sufficiently large build to access/remove records before reducing capacity.

Each record has only two 59-byte state arrays plus compact report/command status.
The active machine and its full-size template/report buffers exist once globally,
not once per radiator. Host ABI measurements: record 256 bytes, registry 2064
bytes at eight; two static storage work buffers total 1694 bytes. Increasing
capacity adds one record plus 210 wire-buffer bytes per slot. Storage buffers
live in BSS. The worker and HTTP stack allocations do not grow with capacity: neither copies
a registry onto its task stack. The radio task retains its 6144-byte stack and
HTTP its existing 8192 bytes. Host compiler stack-usage checks are useful estimates;
optimized host estimates were 1200 bytes for `radio_worker::step`, 160 bytes
for registry persistence and 1904 bytes for the HTTP status handler (individual
frames, not total call-chain high-water marks). Physical task high-water marks
and flash behavior still need board acceptance.

`make test test-http test-capacity` exercises the default and three-slot builds,
including generated full/oversized records and the same two-device HTTP walkthrough.
No synthetic `97`/`5B` defaults are introduced into physical firmware.
