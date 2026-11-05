# Cleverbox

Cleverbox is a local bridge between Purmo/LVI electric radiators and Home
Assistant. It speaks the radiators' 868 MHz CleverTouch RF protocol and exposes
them over a local web page, an HTTP API and MQTT. It needs no cloud account or
vendor service.

Target hardware: Elecram Sub-GHz 855–925 MHz Bridge (ESP32-C6 + CC1101).

## Status

**Experimental firmware for controlled development.** It is not suitable for
unattended deployment.

- Hardware behaviour is not yet verified. Everything below is tested in
  software only, with a host simulation of the radio.
- The firmware assumes a developer is present to read logs, retry, restart,
  reflash and reset radiators. Errors are reported, not recovered automatically.
- Production hardening is deferred; see [accepted risks](docs/production-hardening.md).

| Feature | Software | Hardware |
| --- | --- | --- |
| Receive and decode radiator reports from any controller | Tested | Not tested |
| Monitor radiators without pairing | Tested | Not tested |
| Pair radiators | Tested | Not tested |
| Set target and mode, confirmed by radiator readback | Tested | Not tested |
| Wi-Fi setup, setup access point and BOOT button | Tested | Not tested |
| Device time (SNTP, manual, POSIX time zone) | Tested | Not tested |
| MQTT commands and state | Tested | Not tested |
| Home Assistant discovery and controls | Tested | Not tested |
| OTA firmware updates | Not started | |

## How it works

Cleverbox listens continuously and shows every radiator it hears, whichever
controller it reports to. A heard radiator can be **monitored**: it gets a
record and appears in Home Assistant, and Cleverbox never transmits to it.

To control a radiator, disconnect the Touch E3 and **pair** it: Cleverbox
becomes its RF controller. To change a setting, Cleverbox polls the radiator,
waits for a report, then sends a controller-state frame built from the
[Touch E3 replacement-controller profile](docs/controller-profile.md). A command
counts as `matched` only when a later report shows the requested values.
Commands run one at a time.

Nothing is sent at boot. Only an explicit pairing or command transmits.

## Quick start

### Simulation (no hardware)

Requires a C/C++17 compiler, Make, Python 3, pkg-config and cJSON
(`sudo apt-get install build-essential python3 pkg-config libcjson-dev`).

```sh
make simulate
```

Open http://127.0.0.1:8080. The simulator runs the production firmware code
against two invented radiators, `F1040506` and `F1040507`:

1. Under **Pair a radiator**, enter controller `F1010203` and discovery
   `F1100001` and start pairing. It completes in about two seconds.
2. Under **Time**, set a manual UTC time.
3. Choose a mode and target and press **Send**. The readback appears shortly after.

Settings are stored in `local/simulation.*`; delete them to start over.

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
http://192.168.4.1/ and save your home Wi-Fi. Then use the station IP shown on
the page (also logged on the serial console). To reopen the setup AP, hold BOOT
for five seconds after startup, then release. Do not hold BOOT during reset.
Then follow the [bring-up checklist](docs/hardware.md#bring-up-checklist).

## Documentation

| Document | Contents |
| --- | --- |
| [Architecture](docs/architecture.md) | Components, tasks, registry, pairing, commands, radio errors, storage |
| [HTTP API](docs/http-api.md) | Endpoints and payloads |
| [MQTT](docs/mqtt.md) | Topics, commands, snapshots |
| [Home Assistant](docs/home-assistant.md) | Discovery, control package |
| [Hardware](docs/hardware.md) | Board, wiring, RF profile, flashing, bring-up checklist |
| [Controller profile](docs/controller-profile.md) | Generated state frame, evidence and vectors |
| [RadioLib](docs/radiolib.md) | Pinned version and how it is used |
| [Production hardening](docs/production-hardening.md) | Accepted risks and deferred work |
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
| `make test` | Unit tests: codec, generated frames, pairing, command machine, registry, time, page script syntax (needs Node.js) |
| `make test-http` | Host simulation over HTTP: pairing, commands, queue, registry, radio receive and errors, settings, MQTT and discovery |
| `make test-mqtt-broker` | Real Mosquitto 2.x broker; needs `paho-mqtt==2.1.0` |
| `make RADIOLIB_SOURCE=/path/to/checkout test-radiolib` | Real RadioLib CC1101 driver against a register model; needs CMake |
| `make test-home-assistant` | Home Assistant Core 2026.10.4 with a real broker; needs `host/requirements-ha.txt` |
| `make simulate` | Interactive simulator on http://127.0.0.1:8080 |

CI ([Build](.github/workflows/build.yml)) runs everything except
`test-home-assistant` on pull requests and `main`, and builds the firmware in
`espressif/idf:v5.5.1`. The [Home Assistant workflow](.github/workflows/home-assistant.yml)
runs weekly on `main`, on manual dispatch, and on pull requests labelled
`run-ha`. Re-apply the label to rerun after new commits.

## Scope

In scope: one board, Purmo/LVI radiators, local HTTP, MQTT and Home Assistant
discovery. OTA is planned as a separate milestone.

Out of scope: schedules (Home Assistant owns them), history storage, Matter,
Thread, Zigbee, Ethernet, other boards and cloud services.

## License

[MIT](LICENSE)
