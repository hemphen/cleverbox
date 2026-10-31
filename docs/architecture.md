# Architecture

## Overview

```mermaid
flowchart TB
    browser[Browser] --> http
    ha[Home Assistant] <--> broker[MQTT broker]
    broker <--> mqtt
    subgraph fw [ESP32-C6 firmware]
        http[http_api]
        mqtt[mqtt_bridge]
        net[network_setup]
        time[device_time]
        worker[radio_transport worker]
        proto[radiator_protocol]
        store[device_store]
        nvs[(NVS)]
        http --> worker
        mqtt --> worker
        http --> net
        http --> time
        http --> mqtt
        mqtt --> net
        time --> worker
        worker --> proto
        worker --> store
        store --> nvs
    end
    worker <--> cc1101[CC1101]
    cc1101 <--> radiators[Radiators]
```

HTTP and MQTT never touch the radio. They validate a request, reserve the
target radiator and put the request on the radio worker's queue. The worker is
the only code that drives the CC1101, runs the pairing and command state
machines and writes the radiator registry and operating mode.

## Components

| Component | Responsibility | Depends on |
| --- | --- | --- |
| `radiator_protocol` | Frame codec, CRCs, pairing and command state machines. Portable C, no RTOS or chip code. | – |
| `radio_transport` | CC1101 setup through RadioLib, receive/transmit worker, registry in RAM, request admission | protocol, store, time |
| `device_store` | Registry and RF-mode NVS records, boot-time migration/persistence gate | protocol |
| `network_setup` | Wi-Fi station and setup access point policy, BOOT button, Wi-Fi credentials store | store |
| `device_time` | SNTP, manual UTC, POSIX time zone, command clock | store |
| `mqtt_bridge` | Broker settings, command routing, results, state snapshots, Home Assistant discovery | transport, network |
| `main` | Startup, HTTP handlers (`http_api.c`), embedded page (`index.html`) | all |

Dependencies: ESP-IDF 5.5.1 (FreeRTOS, Wi-Fi, HTTP server, NVS, SNTP, ESP-MQTT)
and RadioLib 7.8.1 pinned to commit `2325a38a89dd0f0e402c19f67fe910111da1e246`.
Only the CC1101 driver is enabled. See the [dependency evaluation](radiolib.md)
for compatibility, real-library tests, resources and hardware limits.

## Tasks and shared state

| Task | Stack | Priority | Loop | Owns |
| --- | --- | --- | --- | --- |
| `radio_rx` | 6144 B | 5 | every 1 ms tick | CC1101, registry RAM, pairing and command machines, registry writes |
| HTTP server | 8192 B | 5 | per request | nothing; admits work |
| `mqtt_bridge` | 8192 B | 3 | every 100 ms | MQTT settings, client lifecycle, sessions, tokens, discovery journal |
| ESP-MQTT client | 6144 B | 5 | event-driven | socket I/O; callbacks only copy input |
| `network_setup` | 4096 B | 3 | ≤ 20 ms queue wait | Wi-Fi mode, credentials, AP policy, BOOT button |

Shared radio state, passive caches, mode changes and admission are guarded by a
task mutex. Template validation and nonblocking queue calls do not disable
interrupts. The mutex is released before NVS or radio I/O. Requests pass to the worker through a FreeRTOS queue of
`capacity + 1` entries. MQTT status and input use a second spinlock. Network
and time status use mutexes. NVS, JSON and network I/O never run inside the radio
mutex. A separate mode-store mutex serializes bootstrap retries and explicit mode
writes; only that small persistence boundary holds a task mutex during NVS I/O.

## Operating mode

Fresh installations start **listen-only**. On first boot the decision is saved
in `rf_mode`. Startup snapshots the migration decision before starting time,
network or other settings owners. A shared gate runs before every registry,
Wi-Fi, timezone, MQTT or discovery-journal mutation. If the first mode write
fails, no settings write can proceed until a retry has durably established that
same boot decision. A fresh registry load does not create an empty registry.
Retries never re-scan settings created during the current boot as migration
evidence. A forced initial retry also avoids trusting a failed write's cached
same-value NVS result. It is serialized against explicit mode saves so it cannot
overwrite a successfully enabled mode.
An upgrade without that key retains **control-enabled** if any prior application
key exists, including an empty registry. This preserves existing installations;
select listen-only explicitly before passive commissioning on an upgraded unit.
Corrupt or unreadable mode/settings detection fails closed for RF and reports a
storage error without erasing or replacing the bad record. An explicit STA mode
save can repair it. A known existing invalid mode record still permits Wi-Fi
and other-key repair without replacing that record; its presence prevents
absent-key migration. Unknown record presence blocks settings writes. Read-only
diagnostics and RAM Wi-Fi trials remain available while persistence is gated.
A failed initial mode error keeps RF closed for the current boot even after a later settings save establishes the initial record;
clear that reported error through an explicit STA mode save or reboot. Wi-Fi
forget never clears the operating mode.

Mode changes are explicit STA-only requests, queued to the existing worker.
They reserve admission under the task mutex and are refused while pairing,
any queued/active command, another mutation or TX is outstanding. Existing
bounded work finishes normally; retry the transition explicitly. No work is
discarded, suspended or resumed by a mode change. While mode persistence is
pending, new pairing and commands are refused. Failed writes close RF, report
`failed` and persistence uncertainty, and may leave old or new mode bytes in
flash. Only a verified successful write reports `succeeded`.

HTTP and MQTT use shared admission. The worker also checks mode immediately
before physical TX and marks TX in progress under the same mutex. Mode admission
cannot pass while that flag is set. Thus no transmission can begin after
listen-only is reported active. Enabling control only saves mode; it does not
pair, poll, adopt a template or create RF work. Network/MQTT events do not change
mode. Boot never resumes RF work.

## Passive commissioning

The production codec decodes reports regardless of destination controller.
A RAM observation cache uses the radiator's source RF address as identity and
retains the latest decoded readings, destination controller and monotonic
receipt time. Five minutes marks an observation stale. No heating or energy
fields are added. Observations do not create pairing, authorize commands or
produce MQTT/HA discovery automatically. Explicit selections are described below. Paired command machines still require both their
saved source and destination addresses to match.

A separate RAM cache retains CRC-valid, structurally valid 59-byte controller
states by radiator destination, from any source controller and before pairing.
Reports, acknowledgements and unknown/malformed frames cannot overwrite them.
Capture uses the codec's existing template validator; active learning retains
its strict paired-controller source rule. Passive capture neither changes
active learned/saved templates nor writes flash.

Each cache holds `2 × CLEVERBOX_RADIATOR_CAPACITY` entries (default 16). A known
address updates in place. Otherwise use the first unused slot, then replace the
least recently received/captured entry; timestamp ties choose the lowest slot.
Caches are independent: report traffic cannot evict templates. Full flags and
saturating eviction counters make the bounds visible. They allocate and write
no flash per packet. Entries and capture versions clear on reboot. External
identities are RF addresses, never reusable cache indices.

Every capture gets a version made from a random 64-bit boot nonce and a strictly
increasing 64-bit sequence. Even identical recaptures change it; sequence
exhaustion refuses further capture rather than wrapping. Explicit adoption
requires that version and radiator address, a matching paired ID, STA access,
and `replace:true` when a saved template exists. Missing/replaced/evicted or
old-boot versions are rejected. Admission copies the exact selected bytes into
the existing registry save request under the mutex; later traffic cannot alter
the queued snapshot. The persistence result names its ID/version. On success,
adoption saves that template and clears the learned RAM override so the selected
bytes become the active saved template. On failure the previous templates remain.
Adoption works in listen-only and sends nothing. Command construction still
rewrites the source to the paired controller; captured sources grant no control.
Manual hex import remains available.

## Passive Home Assistant monitoring

A separate selection list holds `RADIO_MONITOR_CAPACITY` addresses and names
(equal to `CLEVERBOX_RADIATOR_CAPACITY`, default 8). The radio worker persists
explicit select, rename and remove actions in `ha_monitors`, separately from
pairings/templates. Selection requires a decoded report in the general passive
cache or an existing selection. All actions use the full four-byte RF address;
cache slots and destination controllers never identify a device.

Each selection has a dedicated RAM observation updated directly by production
receive code, regardless of destination controller. General cache eviction
cannot stop it updating. These readings clear at reboot. There is no packet
allocation, packet history or per-report flash write. When capacity is full,
selection returns `409`; remove a selection explicitly to free space.

Global HA discovery enables publishing of selected observations and their
read-only discovery entities. Disable stops passive snapshots and cleans up
entities but preserves selections. See [entities and lifecycle](home-assistant.md#passive-monitoring).

An already paired address cannot be selected. An explicit inclusion reply from
a monitored address ends that pairing attempt before bind with `0x7306`.
Remove monitoring, disconnect Touch E3, then start pairing again explicitly.
The discovery address is not necessarily the final radiator address, so an
explicit pairing attempt can already have sent its offer. Passive operations
never send RF. Pairing removal never creates a passive selection; selection
removal never changes pairings or saved templates. No automatic conversion,
mode enable, template adoption or command replay occurs.

The shared mode-persistence gate runs before every selection write. Save uses
a versioned blob with generation, commit and independent handle readback. RAM
changes only after verified success. A failed write reports `failed`, storage
error and uncertainty; old RAM selections continue this boot. Further monitor
mutations stop until reboot, which loads whichever record is durable. Corrupt,
unknown or oversized records are reported, not truncated or overwritten.

Default-capacity host layout compared with main `4c5861d`: static BSS grows by
2960 B (GNU `size`, includes alignment), with a further 48 B in data. This
includes two 304 B selection lists (live and prospective), eight 72 B monitored
observations, two 303 B selection wire buffers, and the expanded discovery
journal/labels. General caches remain 2304 B, the nine-entry queue remains
176 B per request, and command machines do not grow. These are host layout
measurements, not ESP heap/stack high-water measurements. Serialized selection
storage is at most 303 B; the journal grows from 72 B to 264 B, a combined
495 B increase before NVS overhead. Firmware size is reported in the PR build
evidence. No new tasks, timers, queues or dynamic persistent collections exist.

Genuine reception alongside Touch E3, independent RF silence, real report
interpretation/freshness, power-cycle retention and sustained receive
responsiveness remain [hardware checks](hardware.md). Simulator/HA success does
not establish physical heating behavior.

## Radiator registry

Each radiator has a record with:

- **ID**: nonzero 32-bit integer, assigned in increasing order, never reused.
  All interfaces address radiators by ID; names are labels only.
- **Name**: 1–32 UTF-8 bytes, no control characters. New records are named
  `Radiator`.
- **Pairing**: controller address and radiator address (4 bytes each).
- **Saved template** (optional): see [Templates](#templates).

Persisted: ID, name, pairing, saved template. Everything else is RAM-only and
cleared at boot: learned template, latest report, command state and results.
Boot loads the registry and starts receiving. It never transmits.

Capacity is `CLEVERBOX_RADIATOR_CAPACITY` (default 8). Set it for every
component, in both C and C++ flags; on the host, for example
`make CPPFLAGS=-DCLEVERBOX_RADIATOR_CAPACITY=3 test`. A stored registry larger
than the build's capacity blocks all changes
(`registry_problem: stored_registry_exceeds_capacity`) and is never truncated.

Changes to the registry (rename, remove, save or clear a template) run one at a
time through the worker. A record with an outstanding command cannot be removed.
Removing a record does not unpair the physical radiator.

## Templates

A template is a CRC-valid 59-byte controller-state frame whose destination is
the radiator's address. Commands are built from it.

**Selection** at command admission, first match wins:

1. `state` supplied in the HTTP command (one command only; not persisted)
2. Learned template (RAM)
3. Saved template (NVS)

With none available the command is refused.

**Learning.** The worker stores, in RAM, the latest controller-state frame
whose source is the record's controller address and whose destination is its
radiator address. Learning never writes flash and never sends anything.
Cleverbox does not receive its own transmissions, so for a radiator paired by
Cleverbox nothing ever sends a learnable frame. Import a template for those
radiators (`/api/controller-state`, action `import`); the import checks
structure, CRCs and destination, and accepts any source.

**Saving.** `save` copies the learned template to NVS. `import` validates and
saves a supplied frame. `clear` removes both saved and learned templates.

**Frame construction.** Offsets include the length byte.

| Bytes | Field | Value sent |
| --- | --- | --- |
| 0, 5 | Length `38`, marker `C6` | Fixed |
| 1–4, 6–9 | Source, destination | Controller and radiator address from the pairing |
| 11–12, 17, 19–20 | `45`, `3B`, `4C` | Template |
| 14–15 | `4B` measured temperature | Copied from the fresh report, including `084C` (unavailable) |
| 22–24, 26–29 | `8E`, `CC` | Template |
| 31–33 | `97` | Echo of the fresh report's `DF`: byte 0 copied, and `DF` byte 2 bit 0 (heating now) into byte 2 bit 0. Byte 1 and the rest of byte 2 from the template |
| 35–37 | `94` target and mode | Raw target `320 + 18 × °C` rounded (tenths of °F); Off sends raw 320. Mode byte 0 Comfort, 1 Off, 2 Frost, 3 Eco |
| 39–42 | `E1` | Copied from the fresh report |
| 44–47 | `DA` | Zero |
| 49–50 | `5B` Boost time | Template. Minutes; the next Boost's duration, or the time left during a Boost |
| 52–54 | `B2` clock | Local hour, minute, second (see [Time](#time)) |
| 55–56 | Inner CRC | CRC-16/MODBUS over bytes 1–54, little-endian |
| 57–58 | Outer CRC | CRC-16/CMS over bytes 0–56, big-endian |

Non-Off targets must be 5–30 °C. Zeroing `DA` and echoing `DF` in `97` match
the controller frames in the protocol reference. Copying `E1` from the report is
an assumption. None of this is confirmed on hardware.

The controller holds the Boost time and counts it down during a Boost.
Cleverbox has no Boost mode and never changes `5B`, so the template's value only
sets the next Boost's duration. A template captured during a Boost carries the
time then left.

## Pairing

One pairing runs at a time, and only when no command is outstanding. Commands
are refused while pairing runs.

| Phase | Action | Ends when |
| --- | --- | --- |
| `offer` | Send offer to the discovery address | Sent; fails if not sent within 5 s of admission |
| `waiting_reply` | Wait for an inclusion reply addressed to the controller | Reply arrives (radiator address learned) or 5 s pass |
| `bind` | Send bind to the radiator 1 s after the reply | Sent; same 5 s deadline as the reply wait |
| `waiting_ack` | Wait for the radiator's inclusion ack | Ack arrives or 5 s pass |
| `complete` | Save the record | Saved, or save fails (retry with action `save`) |

Terminal phases: `complete`, `timeout`, `cancelled`, `error`. There are no
automatic retries. A reply from an already registered radiator address fails
the attempt before bind (`duplicate: true`). Cancel stops remaining steps; a
bind already sent stays sent. Addresses must share a family byte (`F1` or `D0`)
and the controller address must differ from the discovery address.

## Command delivery

Each radiator has at most one outstanding command. Commands to different
radiators run independently.

| Phase | Meaning | Next |
| --- | --- | --- |
| `poll` | Poll queued | Sent → `waiting_report` |
| `waiting_report` | Waiting for a report from the radiator | Report → `sending` |
| `sending` | State frame queued, built from template and that report | Sent → `waiting_readback` |
| `waiting_readback` | Waiting for a report showing the requested target and mode | Match → `matched`; mismatch after attempt 1 → `sending` |
| `matched` | A report showed the requested target and mode | Terminal |
| `timeout` | A wait expired | Terminal |
| `error` | Transmit or radio failure | Terminal |

Rules:

- Every wait is 150 s. The first deadline counts from admission, including time
  in the queue. Each successful transmission starts a new 150 s wait.
- At most two state frames are sent. A second one is sent only after a new
  mismatching report; there are no timed retransmissions.
- After `timeout` or `error`, a later matching report changes the result to
  `matched` without sending anything.
- `matched` means the radiator reported the requested values. It does not prove
  the command caused them or that heating changed.
- Commands are not persisted or resumed after reboot.

**Transmit scheduling.** Each worker iteration takes at most one queued request,
advances all deadlines and services reception first. A partial frame in
software, bytes in the FIFO or an unstable byte count postpone transmission.
Otherwise the worker sends one ready frame, pairing first, then commands in
round-robin order. Postponing never extends a deadline. Transmission blocks
until done and then restores receive mode. This is half-duplex ordering, not
collision avoidance: a frame can start on air after the last FIFO check.

## Receive path

The worker polls the CC1101 FIFO over SPI every 1 ms tick; interrupts are not
used. It reads `RXBYTES` until two consecutive reads agree (at most nine reads)
and always leaves one byte in the FIFO while a frame is incomplete (TI erratum
SWRZ020E). A frame is length byte `L` (≥ 11) plus `L + 2` bytes, up to 258 bytes.
After a complete frame, an overflow, an invalid count or 100 ms without
completion, the worker flushes the FIFO and restarts reception.

Each valid frame updates global diagnostics (`last_frame`, `report`). A report
whose source and destination match a record's radiator and controller addresses
becomes that radiator's observation and feeds its command machine. Observations
older than 5 minutes are marked `stale`.

If the CC1101 cannot be set up at boot, HTTP stays available and `ready` is
false. If restarting reception fails at runtime, every outstanding command ends
with `error` and the radio task stops until reboot.

## Time

Commands carry the local time of day (`B2`). Time is usable only after an SNTP
sync or a manual UTC set during the current boot; no RTC or stored time is
trusted. Without usable time, all physical HTTP and MQTT commands are refused.
Physical HTTP rejects `clock_seconds` explicitly. Only the host simulator
accepts that override for tests.

- SNTP (`pool.ntp.org`) starts on each station IP event and resyncs hourly.
- Manual UTC (`/api/time`, action `manual`) is valid until reboot or the next
  SNTP sync. Accepted dates: 2020–2099.
- The time zone is a POSIX rule (default `UTC0`), stored in NVS.
- Admission records local seconds since midnight and the monotonic time. Each
  state frame adds elapsed whole seconds, wrapping at midnight. Later clock or
  time zone changes do not affect commands already admitted.
- All deadlines and ages use the monotonic timer, so clock changes never expire
  or restart work.

Supported time zone syntax (max 95 bytes):
`STDoffset[DST[offset],Mm.w.d[/time],Mm.w.d[/time]]`. Names are 3–10 letters.
The offset sign follows POSIX, so `CET-1` means UTC+1. Sweden:
`CET-1CEST,M3.5.0/2,M10.5.0/3`.

## Network

The setup access point uses SSID `Cleverbox`, WPA2 password `cleverbox`,
channel 1, 2 clients and http://192.168.4.1/. The password is public and fixed.
It grants Wi-Fi setup and diagnostics only ([HTTP boundary](http-api.md#interface-policy)).

| Event | Behaviour |
| --- | --- |
| Boot with absent Wi-Fi key | Setup access point on |
| Boot with valid saved credentials | Connect using STA only; no AP window |
| Wi-Fi key corrupt or unreadable | Report `storage_invalid`; no automatic AP, erase or overwrite; BOOT still opens recovery |
| Connection attempt | 15 s deadline; 5 s between attempts; 3 attempts per batch, then 60 s pause; no new AP access |
| Station loses connection or IP | Clear station IP; reconnect after 5 s; do not open AP |
| BOOT held ≥ 5 s then released | Open recovery and keep it open; station and admitted commands unaffected |
| Successful explicit Wi-Fi save | End recovery; close an already open AP after 60 s, even if STA disconnects during that grace period |
| **Finish recovery** | Close AP after 60 s regardless of STA or storage state; UI confirms that the current access path may disappear |
| **Forget home Wi-Fi only** | Erase only the Wi-Fi key and retain recovery access, including after a failed erase |

Starting a replacement trial keeps an already authorised AP session open.
Failed credentials or a failed save, including uncertain persistence, retain
that access through fallback reconnects. Errors do not open a new AP for a
station-only session. A new trial or BOOT cancels a pending AP closure; ordinary
reconnects do not. AP opening retries at one-second intervals after a platform
failure. Reboot clears deliberate access; the saved key decides startup policy.

The adapter loads credentials before starting Wi-Fi. It resets STA through
`NULL`/`STA` when AP is closed, or `AP`/`APSTA` while recovery is authorised.
Retry and stop paths never enable AP as a side effect. Wi-Fi driver NVS is
disabled: the application owns the Wi-Fi key. If NVS partition initialisation
fails, startup logs the error and continues to diagnostics and BOOT recovery
without erasing flash; persistent changes may remain unavailable.

New credentials are tried from RAM and written only after DHCP succeeds. A
failed trial keeps the previous credentials. Only WPA2-personal 2.4 GHz networks
are supported (SSID 1–32 bytes, password 8–63 printable ASCII characters).

The button is GPIO9 (active low, pull-up), debounced for 40 ms. A press held at
boot does not count; the button arms after a stable release. Holding BOOT
during reset can enter ROM download mode because GPIO9 is a strapping pin.

No network or time event creates, cancels or replays an RF command.

## Storage

All keys live in NVS namespace `cleverbox`. Nothing erases the NVS partition
automatically.

| Key | Layout | Written when |
| --- | --- | --- |
| `rf_mode` | v1: version (1), control-enabled flag (0 or 1); 2 bytes | Initial fresh/upgrade decision and explicit STA mode saves |
| `registry` | v1: version (1), count (LE16), next ID (LE32), then per record: ID (LE32), name (33, NUL-terminated), controller and radiator address (4 + 4), saved flag (1), template (59). Size `7 + 105 × count`. | Pairing saved, rename, remove, template save/import/clear |
| `pairing` | Legacy v1: version, controller, radiator (9 bytes). Read once for migration, never written. | – |
| `control_state` | Legacy v1: version, addresses, template (68 bytes) or 9-byte clear marker. Read for migration only. | – |
| `wifi` | v1: version (1), SSID (33), password (64) | Wi-Fi saved or forgotten |
| `timezone` | v1: version (1), NUL-terminated POSIX rule | Time zone saved |
| `mqtt` | v1: version, flags, port (LE16), host (97), username (65), password (129); 295 bytes | Broker configured, enabled, disabled or forgotten |
| `ha_monitors` | v1: version, count (LE16), generation (LE32), then address (4) and name (33) per selection. `7 + 37 × count` bytes. | Explicit monitor select, rename or remove |
| `ha_discovery` | v2: version, enabled, 2 zero bytes, generation (LE32), then `4 × capacity` tagged identities (LE64). High word 0: paired ID; 1: passive full RF address in low word. Existing v1 paired journal remains readable and upgrades on its next write. | Discovery enabled/disabled; topic ownership added or acknowledged cleanup removed |

Write policy:

- The registry is replaced as one blob. If the commit fails, the previous bytes
  are restored. RAM changes only after a successful commit.
- Wi-Fi, time zone and MQTT writes erase, rewrite and read back the key. A
  failed Wi-Fi write also restores and checks the previous value. If the result
  cannot be confirmed, status reports `persistence_uncertain`; the next boot may
  load the old, new or no value.
- Corrupt or unknown records are reported and left untouched.
- Migration: if `registry` is absent and `pairing` is valid, boot creates
  record 1 (`Radiator 1`) and commits `registry`. Legacy keys stay. An existing
  `registry`, even empty, always wins, so removed radiators do not come back.

## Security

- HTTP has no authentication and no HTTPS. Station-interface control assumes
  a trusted LAN: every host able to reach that interface can change settings
  and control radiators. Do not forward the port or expose it to an untrusted LAN.
- AP access is deliberate after configuration and restricted to Wi-Fi setup
  and diagnostics by the central [endpoint allowlist](http-api.md#interface-policy).
  A joined AP client can replace/forget Wi-Fi and inspect household diagnostics.
  BOOT authorises a recovery session, not an individual user.
- These boundaries address issue #20's automatic AP exposure and physical clock
  bypass. They do not provide HTTP authentication or complete its broader
  authorisation item. OTA and its authorisation remain separate decisions.
- MQTT uses TCP without TLS; credentials cross the network in clear text.
- NVS encryption, flash encryption and secure boot are off.
- Passwords are never returned by the API and Wi-Fi driver logging is
  suppressed. The Wi-Fi password is cleared from request buffers after use.
- Content-Type must be exactly `application/json`, which blocks simple
  cross-site form posts.

## Host simulation

The host build compiles the production protocol, radio worker, registry, HTTP
handlers, network policy, time logic and MQTT bridge into a shared library.
`host/serve.py` serves it on 127.0.0.1 and runs everything on one thread.

Replaced on the host: RadioLib and the CC1101 (a FIFO model), FreeRTOS queues
and locks, the monotonic clock, NVS (file-backed), the Wi-Fi driver and the
MQTT client (fake, or real sockets with `--real-mqtt`).

Two invented radiators answer offers, binds, polls and valid state frames with
replies and reports. Their report writer uses its own CRC code. The synthetic
fixture and its template values exist only in the simulator and tests.

Because the simulation is single-threaded, it does not exercise task
scheduling, lock contention, stack depth or RF timing. Loopback tests exercise
real `getsockname`/`getpeername` (including IPv4-mapped IPv6 sockets),
production dispatch and handlers, but substitute
netif addresses and Wi-Fi events. They cannot prove ESP/lwIP ingress routing,
AP/STA isolation, DHCP event timing, phone access or GPIO behaviour.
`make test-http` also compiles `http_api.c` without `CLEVERBOX_HOST`, tests clock
override rejection and device-time admission, and retains host-only RF
observability outside the physical API.

`serve.py` options: `--port` (default 8080), `--storage`, `--real-mqtt`,
`--test-clock`, `--test-interfaces` (extra loopback destinations for AP/unknown
classification). Storage defaults to `local/simulation-pairing.bin`; each NVS key
gets its own file next to it (`.registry`, `.wifi`, `.mqtt`, `.timezone`,
`.ha_discovery`, `.ha_monitors`, `.rf_mode`). Stop the simulator and delete these files to reset.

## Constants

| Value | Setting |
| --- | --- |
| 150 s | Command report and readback wait |
| 2 | State frames per command |
| 5 min | Observation `stale` threshold |
| 5 s | Pairing reply and ack waits; offer deadline |
| 1 s | Delay before bind |
| 100 ms | Incomplete-frame receive timeout |
| 1 ms | Radio worker poll interval |
| 100 ms | MQTT owner tick |
| 5 s | MQTT state snapshot interval, reconnect pause, SUBACK and PUBACK timeouts |
| 15 s | Wi-Fi attempt deadline |
| 60 s | Access point grace period; pause between connection batches |
| 5 s | BOOT hold for recovery |
| 8 | Default radiator capacity |
