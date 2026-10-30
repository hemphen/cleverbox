# Radiator registry and independent delivery

The registry and independent per-radiator delivery are implemented. Each radiator
may have one outstanding command, from admission through worker handoff, report
waits and bounded retry. Another command for that radiator returns HTTP 409.
A command for an idle radiator can proceed while others wait for reports. Pairing
remains exclusive: it cannot start while any command is outstanding, and commands
are rejected throughout pairing.

Every configured radiator owns its identity, name, pairing, saved state, learned
state, observations and latest command result. Completing B does not erase A's
result. Reports and controller frames are routed by both source and destination;
selection is never a routing input. Each active command machine owns a compact admitted
state snapshot and fresh-report/readback exchange. The worker chooses one ready
frame per tick in round-robin order and restores receive mode after every TX.
Reception is serviced before TX selection; partial/FIFO-only/uncertain reception
defers TX without consuming attempts or advancing the round-robin cursor. Ready
work remains bounded by its deadline. See the
[transport policy](../firmware/README.md#receivetransmit-arbitration).
Report waits never reserve the CC1101. After timeout/TX failure,
matching late reports can reconcile the per-device result without another send.
There is no per-radiator backlog or replacement policy.
If the radio cannot restore reception and the worker stops, every admitted
command, including those still queued for handoff, receives a terminal error.
No command is resumed or transmitted automatically after restart.

## HTTP identities

`GET /api/radiators` (also `GET /api/radio`) lists `radiators`, `capacity`,
`registry_error`, `registry_problem`, `exchange_id` (pairing identity), `active_commands`, `mutation_pending` and
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
| `POST /api/command` | `{"id":1,"mode":0,"centi_c":2050}`; optional per-command `state` retains import precedence. |

Normal commands use [device time](time.md) and reject an unset boot. Explicit
`clock_seconds` remains a documented compatibility/test override; the normal page
omits it. Time setup and synchronization do not mutate radiator records.

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
`inclusion` for pairing and per-device `command` for delivery. Pending work is bounded by a worker queue of capacity plus one and one
local-storage reservation. An active record cannot be removed, including before
the worker dequeues it. The queue holds at most one command for each radiator.
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

Each record retains two 59-byte state arrays, compact report/command status and
an admission reservation. The worker owns one compact control machine per configured
capacity slot: 59 state bytes and up to 64 report bytes, rather than retaining two
258-byte frame buffers per command. Full envelopes are temporary codec/TX/RX values.
The request queue has `capacity + 1` slots of 136 bytes on the tested host ABI;
this bounds admission during worker handoff without allowing a second command for
the same device. At capacity eight, the host ABI measured record 304 bytes (including the transient MQTT result mailbox),
registry 2448 bytes, worker 1960 bytes and queue payload at most 1224 bytes
(plus RTOS queue metadata). At capacity three, worker size was 944 bytes.
Each additional slot adds 304 registry bytes, about 203 worker bytes and 136
queue payload bytes, plus 210 bytes in the two static storage wire buffers.
The storage buffers and capacity-dependent worker live in static memory on
firmware. The radio task keeps its 6144-byte stack for temporary frames and
function calls; HTTP keeps its 8192-byte stack. At capacity 32, the host ABI
worker measures 6856 bytes, which cannot fit on that task stack. Physical task
high-water marks, actual queue allocation and flash durability need board acceptance.

`make test test-http test-capacity` exercises the default and three-slot builds,
including generated full/oversized records and the same two-device HTTP walkthrough.
No synthetic `97`/`5B` defaults are introduced into physical firmware.
