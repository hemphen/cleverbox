# Local HTTP simulation

No ESP32, CC1101, radiator, ESP-IDF, Python package or Node server is needed.
The host build uses a C/C++17 compiler, Make, Python 3, pkg-config and cJSON.
On Debian/Ubuntu install prerequisites with
`sudo apt-get install build-essential python3 pkg-config libcjson-dev`.
Node.js is additionally needed for the existing UI tests. On macOS the equivalent
cJSON dependency is available through `brew install cjson pkg-config` (Linux tested).

From the repository root, start with this one command:

```sh
make simulate
```

Open **http://127.0.0.1:8080**. The application binds only to loopback. Stop with
Ctrl-C, and run the same command to restart. Accepted connections have a 0.5-second socket inactivity timeout so idle or
incomplete requests cannot indefinitely stop HTTP service and the simulated clock.
Application access remains single-threaded. Only one process should use a given
storage file. To change the port/storage path after building, run
`python3 host/serve.py --port 8081 --storage local/another-pairing.bin`.

## Two-radiator browser walkthrough

1. On fresh storage, choose inclusion/fixture device **F1040506**, then **Choose
   simulated device**. The page supplies invented controller `F1010203` and
   discovery destination `F1100001`. Start pairing and wait about two seconds for
   inclusion to be saved. No physical inclusion mode is needed.
2. Select the added radiator, name it **Living room**, save the name and check
   local configuration status. With the import empty, send a command: missing
   controller state is visibly rejected. Inject the **synthetic controller-state
   fixture**, then **Save learned state / retry**. Wait for save success. This
   separate original-controller stand-in is not a radiator report.
3. Choose inclusion/fixture device **F1040507**, apply that choice, and pair it.
   Select the new record, name it **Bedroom**, inject its fixture and explicitly
   save it. Its opaque template bytes differ from Living room's. The selector
   controls simulator inclusion/fixture injection only, not application routing.
4. Select Living room, set a target/mode and send. Requested settings appear
   before readback. While waiting, a request for Bedroom receives busy (the UI
   disables send). After matching readback, select Bedroom and send a different
   target/mode. Switching back retains Living room's observations and result.
5. Stop with Ctrl-C and restart `make simulate`. Both identities, names, pairings
   and saved templates remain. Commands are idle, observations/learned state are
   empty and TX diagnostics are empty. Send to either using saved state and a
   fresh report, with no new fixture or manual import.
6. Remove Living room's **local record**. Restart; Bedroom remains and can still
   be controlled. Removal does not reset/unpair a physical radiator. Re-adding
   allocates a new identity. Pairing an already configured simulated radiator
   reports duplicate without overwriting it.
7. Choose delayed responses (2.5 seconds) to inspect the intermediate requested
   and observed states. Silence times out after the production 150-second wait.
   ACK-only supplies the initial poll report and state ACK but no matching
   readback, so it also times out. `silence_a` lets B complete while A remains
   pending and then times out. There is no second command backlog per radiator.

The browser stores selection locally and sends an explicit identity on each
mutation/command. Switching selection clears manual import text and the previous
observation display. A second browser's selection has no routing effect. Global
receive diagnostics are separately labelled and include all received addresses.

## HTTP walkthrough

Start `make simulate`, then use JSON requests from another terminal. On fresh
storage, the first two successful records receive IDs 1 and 2; otherwise obtain
actual IDs from `GET /api/radiators` and the pairing response. Wait for each
inclusion/storage result before the next step:

```sh
base=http://127.0.0.1:8080
post() { curl -sS -H 'Content-Type: application/json' -d "$2" "$base$1"; }
post /api/simulation '{"action":"device","index":0}'
post /api/pairing '{"action":"start","source":"F1010203","destination":"F1100001"}'
curl -sS "$base/api/radiators" # Wait for inclusion.saved.
post /api/radiators '{"action":"rename","id":1,"name":"Living room"}'
post /api/simulation '{"action":"fixture"}'
post /api/controller-state '{"action":"save","id":1}'
curl -sS "$base/api/radio?id=1" # Wait for command.save_status=succeeded.
post /api/simulation '{"action":"device","index":1}'
post /api/pairing '{"action":"start","source":"F1010203","destination":"F1100001"}'
curl -sS "$base/api/radiators" # Wait for inclusion.saved.
post /api/radiators '{"action":"rename","id":2,"name":"Bedroom"}'
post /api/simulation '{"action":"fixture"}'
post /api/controller-state '{"action":"save","id":2}'
curl -sS "$base/api/radio?id=2" # Wait for command.save_status=succeeded.
post /api/command '{"id":1,"mode":0,"centi_c":2050,"clock_seconds":43200}'
post /api/radiators '{"action":"select","id":2}' # Stateless second-client selection.
curl -sS "$base/api/radio?id=1" # Wait for command.phase=matched.
post /api/command '{"id":2,"mode":3,"centi_c":1550,"clock_seconds":43200}'
curl -sS "$base/api/radio?id=2" # Wait for matching readback; A's result remains.
```

`post /api/radiators '{"action":"remove","id":1}'` removes local setup after
commit. `GET /api/radio?id=1` then returns 404; record 2 survives restart.
For complete schemas and busy/ambiguous behavior see the [registry API](registry.md).

## Storage and migration

The host's file-backed NVS subset keeps the registry at
`local/simulation-pairing.bin.registry`, using production storage validation.
Existing `local/simulation-pairing.bin` (pairing) and `.state` (saved state) migrate
without changes to those files. The new registry is authoritative, including when
empty. Stop before manipulating storage; only one process may use it. To reset a
simulation intentionally, remove the `.registry` file **and** both legacy files.
Removing only the registry can intentionally retrigger legacy migration.

Unknown/corrupt/oversized registry records are visible and block writes rather
than resetting/truncating devices. A failed migration retains legacy setup for
retry. Passive reception writes no flash; boot sends nothing. The two simulated
radiators each start at 21 °C target/Comfort after process restart, with fixed
measured temperature near 20 °C; the simulated physical settings are not persisted.

## What runs

- The **production** C codec, pairing and command state machines, and transport
  admission/receive worker run unchanged in behavior. The worker now exposes one
  iteration for both its FreeRTOS loop and the host's single-threaded event loop.
- The HTTP API handlers were moved from `main.c` to `http_api.c`. Both builds
  compile those same handlers and use the same page. The host HTTP shim implements
  only their request/response calls; Python handles loopback sockets and page
  delivery, never protocol decoding, pairing or command decisions.
- Host-only queue, FIFO, monotonic-clock and NVS facades replace platform I/O.
  No RadioLib driver or ESP-IDF runs on the host. `CLEVERBOX_HOST` is set only by
  the host Make targets; physical firmware has no simulation endpoint or fixture.
- Two deterministic invented radiators recognize offer/bind/poll packets including
  source/destination and payload. The inclusion selector chooses one responder. It responds to valid state layouts/targets/modes with short ACKs and
  subsequently generated reports. The report writer has its own CRC calculation
  and never calls the transmitter's builders. Application of valid state is a
  simulation assumption, not an observation about hardware.
- The explicit fixtures use the published synthetic example and a separately
  sealed second-device variant with distinct opaque bytes. Its opaque `97` and
  `5B` values exist **only in host simulation/tests**. They are not construction
  rules or physical-radiator defaults; see [controller-state.md](controller-state.md).

## Scenarios, clock and tests

Interactive time is **not accelerated**. Normal replies take 300 ms; delayed
replies take 2500 ms. State ACKs take 100 ms. The one-second bind delay and
five-second pairing / 150-second command deadlines retain production values.
Applying a scenario clears queued simulated responses, not application state.
Silence drops responses. ACK-only permits the initial poll report, then supplies
only a short ACK for state TX. There is no background periodic traffic or heating
physics. Fresh mismatching reports can exercise the existing two-attempt bound;
there are no blind timed retries.

```sh
make test test-http test-capacity
```

`test-http` starts real HTTP subprocesses with temporary empty storage and a
controllable clock. Test-only `--test-clock` freezes time and enables
`POST /api/simulation` actions `advance` (`ms`) and `inject` (`frame` hex).
Neither action is available in interactive mode. Ordinary host-only actions are
`device` (`index` 0 or 1), `fixture` for that device, and `scenario` with `name` normal/delayed/silence/silence_a/ack_only.
`silence_a` suppresses replies from invented radiator A only. Start A, then B:
B can receive readback and complete while A awaits its own timeout. Test-clock
injection also exercises interleaved reports, independent bounded retries and
late reconciliation. These results do not establish physical RF or heating behavior.
Injected bytes enter the same FIFO as simulator responses; invalid CRCs and wrong
addresses are rejected by production receive/application handling.

The black-box tests cover two-radiator pairing/naming/saving, explicit identity
routing across client selections, preserved per-device results, interleaved traffic,
distinct opaque bytes, duplicate/full/oversized registry rejection, migration
failures/retries and removal across restart, plus empty-storage pairing, missing state, separate learning,
manual import, explicit save/import/clear, saved-state restart, temporary override,
RAM precedence without flash overwrite, pairing-only compatibility, visible corrupt
storage, requested versus decoded readback, normal/delayed/silent/ACK-only
traffic, the two-attempt bound, corrupt and wrong-address traffic, persistence and
restart without TX. Literal expected offer/bind/poll and complete live-state/report
vectors come from the existing independent reference vectors, not simulator output.
Generated records exercise full capacity; `test-capacity` reruns the same suites
with a three-record build, including a larger stored configuration that cannot fit.
Existing host/UI tests and the ESP32-C6 firmware build also run in CI.

**Limits:** simulation checks application behavior against protocol assumptions.
It does not prove RF reception, transmission compatibility, capture-free setup,
physical flash persistence, thermostat ownership or heating behavior. It does not
resolve construction of `97`/`5B`, simulate RF modulation, a thermostat algorithm,
periodic radiator operation or independent delivery scheduling.
