# Cleverbox

Cleverbox is a local bridge between Purmo/LVI electric radiators and Home
Assistant. It speaks the radiators' 868 MHz CleverTouch RF protocol and exposes
them over a local web page, an HTTP API and MQTT. It needs no cloud account or
vendor service.

Target hardware: Elecram Sub-GHz 855–925 MHz Bridge (ESP32-C6 + CC1101).

## Status

All features below are implemented and covered by host tests. None has been
tested on the board.

| Feature | Software | Hardware |
| --- | --- | --- |
| Listen-only mode, passive inventory and retained template capture | Tested | Not tested |
| Receive and decode radiator reports | Tested | Not tested |
| Pair radiators (one at a time, up to 8) | Tested | Not tested |
| Set target and mode, confirmed by radiator readback | Tested | Not tested |
| Wi-Fi setup, setup access point and button recovery | Tested | Not tested |
| Device time (SNTP, POSIX time zone) | Tested | Not tested |
| MQTT commands and state | Tested | Not tested |
| Selected passive radiator monitoring in Home Assistant | Tested | Not tested |
| Home Assistant discovery | Tested | Not tested |
| OTA firmware updates | Not started | |

## How it works

Fresh installations start listen-only. Upgrades without a saved mode preserve
control behavior. The local passive view identifies reports from any controller
and retains genuine template candidates without pairing or transmitting.
Select observed RF addresses for read-only Home Assistant monitoring without pairing,
then enable global discovery. Follow the [commissioning workflow](docs/http-api.md#passive-commissioning).
Disconnect Touch E3 before explicitly enabling control and pairing.

Cleverbox pairs with each radiator as its RF controller and listens for the
reports the radiator sends about every two minutes. To change a setting it
polls the radiator, waits for a fresh report, then sends a controller-state
frame with the new target and mode. A command counts as `matched` only when a
later report shows the requested values.

Each controller-state frame is built from a **template**: a 59-byte frame for
that radiator captured from real traffic. The fields with no known meaning
(`45`, `3B`, `4C`, `8E`, `CC` and part of `97`) and the Boost time (`5B`) come
from the template; the rest comes from the fresh report, the command and the
clock. See [architecture](docs/architecture.md#templates).

## Known limitations

- No hardware testing yet. RF timing, transmit acceptance and heating behaviour
  are unverified.
- Control needs a genuine same-radiator template. Capture one passively while
  Touch E3 is operating, then adopt it after explicit pairing, or import hex.
  New-controller acceptance still requires hardware validation.
- The HTTP interface has no login. The setup access point uses the fixed,
  published password `cleverbox`. After setup it opens only through deliberate
  recovery (BOOT or Wi-Fi forget), and permits Wi-Fi setup and diagnostics only.
  Full HTTP control requires the station interface on a trusted LAN.
- MQTT uses plain TCP. NVS is not encrypted.
- UI and MQTT commands are refused after each boot until the clock is set by
  SNTP or by hand on the station interface. Physical `POST /api/command`
  rejects `clock_seconds`; only the host simulator accepts that test override
  ([Time](docs/architecture.md#time)).
- If the radio cannot be initialised, or restoring reception fails at runtime,
  the radio task stops until the next reboot. A transmit failure ends only the
  affected command; receive errors are counted and reception restarts
  ([Receive path](docs/architecture.md#receive-path)).

## Quick start

### Simulation (no hardware)

Requires a C/C++17 compiler, Make, Python 3, pkg-config and cJSON
(`sudo apt-get install build-essential python3 pkg-config libcjson-dev`).

```sh
make simulate
```

Open http://127.0.0.1:8080. The simulator runs the production firmware code
against two invented radiators, `F1040506` and `F1040507`:

1. Explicitly enable control in **Passive commissioning**, then choose a simulated device and start pairing. Inclusion completes in about
   two seconds.
2. Under **Device time**, set a manual UTC time.
3. Inject the synthetic controller-state fixture, then **Save learned state**.
4. Send a target and mode. The requested values appear first, the readback
   shortly after.

Restart `make simulate` to check that pairings, names and saved templates
persist while commands and observations do not. Simulation-only controls are
listed in the [HTTP API](docs/http-api.md#simulation-only).

### Board

Build and flash with ESP-IDF 5.5.1, or use the `cleverbox-esp32c6` artifact
from CI:

```sh
cd firmware
idf.py set-target esp32c6
idf.py build
idf.py -p PORT flash monitor
```

Join Wi-Fi network **Cleverbox** (password **cleverbox**), open
http://192.168.4.1/ and enter your home Wi-Fi on first setup. After saving,
move to the home LAN and use the station IP for radiator, time and MQTT setup.
Configured boots and router outages keep the AP off. To recover without serial,
hold BOOT for five seconds after startup, then release. Keep BOOT released
during reset. Then follow the
[bring-up checklist](docs/hardware.md#bring-up-checklist).

## Documentation

| Document | Contents |
| --- | --- |
| [Architecture](docs/architecture.md) | Components, tasks, state machines, storage, timing |
| [HTTP API](docs/http-api.md) | Endpoints, payloads, status fields |
| [MQTT](docs/mqtt.md) | Topics, command protocol, results, broker setup |
| [Home Assistant](docs/home-assistant.md) | Discovery, control package, automations |
| [Hardware](docs/hardware.md) | Board, wiring, RF profile, flashing, bring-up checklist |
| [AGENTS.md](AGENTS.md) | Rules for contributors and coding agents |

The radio protocol is specified in
[clevertouch-protocol](https://github.com/hemphen/clevertouch-protocol/tree/main/docs/radiator-rf)
(implemented against revision `b70fab79300544a62ee2606bd4055cdb81763fd4`;
the `97`, `5B` and `DF` readings follow
`5dc2bcff61a817a574e6fc9df4e9e9c1e5052571`).
The reference decoder is in
[clevertouch-radio](https://github.com/hemphen/clevertouch-radio).

## Development

| Command | Runs |
| --- | --- |
| `make test` | Unit tests: codec, pairing, commands, radio worker, registry, network, time, MQTT store, page script (needs Node.js) |
| `make test-http` | Host suites over HTTP against the production code: registry, network, recovery, time, MQTT, tokens, discovery; physical HTTP branch and interface policy |
| `make test-capacity` | `test` and `test-http` with a 3-radiator build |
| `make test-discovery-limit` | Discovery with an undersized payload buffer |
| `make test-mqtt-broker` | Real Mosquitto 2.x broker; needs `paho-mqtt==2.1.0` |
| `make RADIOLIB_SOURCE=/path/to/checkout test-radiolib` | Real RadioLib CC1101 register/HAL probes; needs CMake ([details](docs/radiolib.md)) |
| `make test-home-assistant` | Home Assistant Core 2026.10.4 with a real broker; needs `host/requirements-ha.txt` |
| `make simulate` | Interactive simulator on http://127.0.0.1:8080 |

CI ([Build](.github/workflows/build.yml)) runs everything except
`test-home-assistant` on pull requests and `main`, and builds the firmware image
in `espressif/idf:v5.5.1`. The
[Home Assistant workflow](.github/workflows/home-assistant.yml) runs weekly on
`main`, on manual dispatch, and on pull requests labelled `run-ha`. Re-apply the
label to rerun after new commits.

## Scope

In scope: one board, Purmo/LVI radiators, local HTTP, MQTT and Home Assistant
discovery. OTA is planned as a separate milestone.

Out of scope: schedules (Home Assistant owns them), history storage, Matter,
Thread, Zigbee, Ethernet, other boards and cloud services.

## License

[MIT](LICENSE)
