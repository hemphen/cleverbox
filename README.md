# Cleverbox

A small, local bridge for Purmo/LVI radiators, targeting the Elecram Sub-GHz
855–925 MHz Bridge (ESP32-C6 + CC1101).

**Status: locally runnable HTTP application with simulated RF traffic; physical
firmware supports reception, a persistent multi-radiator registry with explicit HTTP routing,
explicit controller-state saving, independent per-radiator report-driven commands and
persistent home Wi-Fi setup/recovery, device-managed time and local MQTT transport. No hardware test yet.**

## Run locally without hardware

With a C/C++17 compiler, Make, Python 3, pkg-config and cJSON development package
installed, run from the repository root:

```sh
make simulate
```

Open **http://127.0.0.1:8080**. Pair the two invented radiators one at a time, name each, try sending before
learning state, inject the separate **synthetic controller-state fixture**,
explicitly save it, restart, then select either radiator and change target/mode without injecting it again.
Set explicit manual UTC time under **Device time** (simulation starts unsynchronized).
Observe decoded readback. Choose delayed responses to see
the requested settings before matching readback. See the
[startup prerequisites, walkthrough and limitations](docs/simulation.md).

Simulation uses the production codec, pairing/command state machines, receive
handling and page. It checks application behavior against protocol assumptions;
it does **not** prove RF reception, transmission compatibility, capture-free setup
or heating behavior.

Manual template input is optional once a matching controller-state frame has been
heard for the saved pair or explicitly saved/imported. Passive learning stays in
RAM; explicit saving retains validated state across restart. Fully independent
construction remains
blocked by unresolved fields `97` and `5B`; synthetic bytes are not physical-device
defaults. See the [field assessment](docs/controller-state.md).

## Physical startup and network recovery

Read the USB serial console at 115200 baud for the random per-boot **Cleverbox**
AP password. Join it and open **http://192.168.4.1/**. Enter home Wi-Fi in the
existing page; credentials are saved after DHCP success. Open the station IP
shown on the page/serial from the home network. The protected setup AP closes
60 seconds after connection and reopens on network loss. An uncertain storage
result keeps the AP open and is shown explicitly. **Forget home Wi-Fi only**
preserves radiator setup. See [network setup, recovery and validation](docs/networking.md).

## Product direction

Local HTTP setup/control comes first, with a persistent registry and independent per-radiator delivery and [device-managed time](docs/time.md) implemented.
[MQTT broker setup, commands and state publishing](docs/mqtt.md) are implemented.
[HA discovery](docs/home-assistant.md) automatically adds observation/diagnostic
devices when enabled; a smaller optional built-in package supplies explicit
draft/Apply controls. Next: installation/physical UI usability checks →
endurance/upgrade and hardware acceptance → optional Matter. Interfaces will share the same command handling.
Home Assistant supplies history, schedules, automations and notifications.
No provider account or cloud service is intended to be required in normal operation.

## Scope

This is one embedded appliance, not a general radio gateway or a protocol
research project. We use the existing specifications as working truths and
implement them. Hardware acceptance tests check our implementation; they are not
a prerequisite programme to re-establish every protocol claim.

Thread is a possible later alternative on the same hardware, not part of the
first build. Ethernet/PoE, Z-Wave, other boards, cloud fallback, an onboard history
database and a schedule engine are outside this implementation. Local HTTP is
the first control interface; MQTT and optional Matter will use the same command handling.

## Working here

Read [AGENTS.md](AGENTS.md), the [hardware checklist](docs/hardware.md),
[implementation plan](docs/implementation.md) and [libraries](docs/libraries.md).
The [firmware directory](firmware/README.md) describes the build layout.

Protocol definitions live in
[clevertouch-protocol](https://github.com/hemphen/clevertouch-protocol/tree/main/docs/radiator-rf).
Existing decoder and tooling live in
[clevertouch-radio](https://github.com/hemphen/clevertouch-radio).
Link to these sources rather than copying their documentation or private captures.

Licensed under [MIT](LICENSE).
