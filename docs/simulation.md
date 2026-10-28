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

## Browser walkthrough

1. On a fresh run, the page displays **SIMULATION · no physical radio** and fills
   controller `F1010203` and discovery destination `F1100001`. Click **Start
   pairing**. The simulator replies as invented radiator `F1040506`; within about
   two seconds the page shows the saved pair. No physical inclusion mode is needed.
2. With the manual import empty, click **Send to paired radiator**. The response
   clearly rejects the command because no controller state has been learned.
3. Click **Inject synthetic controller-state fixture**. Wait for the learned-state
   indication. This is a separate original-controller stand-in, **not a radiator
   report**. It passes through the normal receive/learning path and sends nothing.
4. Select **Delayed (2.5 seconds)** and click **Apply response scenario**. Choose a
   new target and mode, leaving the import empty, then send. The command area shows
   the requested settings, the old decoded observation arrives after the poll,
   and a later decoded report matches the new settings. The two displays remain
   separate. Receive diagnostics include the last 16 transmitted packets.
5. Select **Silence** and apply it, then send. The command times out after 150 real
   seconds without a state transmission. **ACK only after state TX** supplies the
   initial report, acknowledges the subsequent state, but gives no matching
   readback: that command also times out and never becomes matched.
6. Restart while a command is pending. Pairing remains; command status is idle,
   observations and learned state are empty, and diagnostics contain no TX.
   Inject the fixture again or manually import same-radiator state before sending.

Pairing is saved to `local/simulation-pairing.bin`, using the production
`device_store` versioned nine-byte record through a file-backed NVS subset.
To repeat from empty storage, stop the app and explicitly remove that file.
Corrupt/unknown records block new pairing rather than silently resetting it.
Learned controller state, requested commands and observations are RAM only.
The simulated radiator starts at 21 °C target/Comfort on each process start;
measured temperature is fixed at approximately 20 °C. Settings are not saved.

## What runs

- The **production** C codec, pairing and command state machines, and transport
  admission/receive worker run unchanged in behavior. The worker now exposes one
  iteration for both its FreeRTOS loop and the host's single-threaded event loop.
- The four HTTP API handlers were moved from `main.c` to `http_api.c`. Both builds
  compile those same handlers and use the same page. The host HTTP shim implements
  only their request/response calls; Python handles loopback sockets and page
  delivery, never protocol decoding, pairing or command decisions.
- Host-only queue, FIFO, monotonic-clock and NVS facades replace platform I/O.
  No RadioLib driver or ESP-IDF runs on the host. `CLEVERBOX_HOST` is set only by
  the host Make targets; physical firmware has no simulation endpoint or fixture.
- One deterministic radiator recognizes literal offer/bind/poll packets including
  addresses. It responds to valid state layouts/targets/modes with short ACKs and
  subsequently generated reports. The report writer has its own CRC calculation
  and never calls the transmitter's builders. Application of valid state is a
  simulation assumption, not an observation about hardware.
- The explicit fixture uses the published synthetic example. Its opaque `97` and
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
make test test-http
```

`test-http` starts real HTTP subprocesses with temporary empty storage and a
controllable clock. Test-only `--test-clock` freezes time and enables
`POST /api/simulation` actions `advance` (`ms`) and `inject` (`frame` hex).
Neither action is available in interactive mode. Ordinary host-only actions are
`fixture` and `scenario` with `name` normal/delayed/silence/ack_only.
Injected bytes enter the same FIFO as simulator responses; invalid CRCs and wrong
addresses are rejected by production receive/application handling.

The black-box tests cover empty-storage pairing, missing state, separate learning,
manual import, requested versus decoded readback, normal/delayed/silent/ACK-only
traffic, the two-attempt bound, corrupt and wrong-address traffic, persistence and
restart without TX. Literal expected offer/bind/poll and complete live-state/report
vectors come from the existing independent reference vectors, not simulator output.
Existing host/UI tests and the ESP32-C6 firmware build also run in CI.

**Limits:** simulation checks application behavior against protocol assumptions.
It does not prove RF reception, transmission compatibility, capture-free setup,
physical flash persistence, thermostat ownership or heating behavior. It does not
resolve construction of `97`/`5B`, simulate RF modulation, a thermostat algorithm,
multiple radiators or periodic radiator operation.
