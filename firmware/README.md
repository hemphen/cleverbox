# Firmware

The HTTP control prototype uses ESP-IDF **v5.5.1**, its HTTP server and the portable C protocol
component. The fixed Elecram board wiring is documented in [hardware](../docs/hardware.md).
The page remains available if radio initialization fails.

```sh
# With ESP-IDF v5.5.1 installed and export.sh loaded:
cd firmware
idf.py set-target esp32c6
idf.py build
idf.py -p PORT flash monitor
```

The console uses the ESP32-C6 native USB Serial/JTAG port.

Join the **Cleverbox** Wi-Fi network using the temporary password printed on
the serial console. Open **http://192.168.4.1**. The password changes each boot.
This bench build starts an access point; home-network provisioning comes later.

The start page prepares pairing offer/bind, poll and target/mode frames.
**Pairing and live target/mode commands transmit. Buttons labelled Preview do not.**
Every preview result is labelled preview. Frame bytes begin at the length byte;
preamble/sync and CC1101 framing belong to the upcoming transport.

## HTTP

`GET /` serves the page. `POST /api/preview` accepts JSON (maximum 1023 bytes).
Invalid requests return 400. A successful response contains
`status: "preview"`, `transmitted: false` and `frame` as hexadecimal.

Offer, bind and poll requests use eight-digit source and destination addresses:

```json
{"command":"offer","source":"F1010203","destination":"F1100001"}
```

These are synthetic reference addresses, not defaults for a real installation.
For an offer, supply a controller source with the candidate's family byte.
The bind preview uses the entered destination. The separate pairing flow below
uses the responding radiator's address automatically; candidate enumeration is
not implemented.

Target changes require a complete CRC-valid 59-byte controller-state frame:

```text
{"command":"target","state":"<controller-state hex>","mode":0,"centi_c":2050}
```

Modes: 0 Comfort, 1 Off, 2 Frost, 3 Eco. Non-Off targets are 5–30 °C
(stage-1 application limits), passed as integer hundredths of a degree.
Off uses the reference's raw target 320. The codec updates tag 94 and both CRCs,
preserving addresses, opaque fields and clock bytes. The preview endpoint remains a frame-editing primitive. Live commands use the
separate report-driven flow below.
No schedules, Boost duration or inferred heating-duty control are implemented.

## Tests and next step

Run `make test` at the repository root for the portable codec. Tests use
synthetic vectors from clevertouch-protocol revision
`b70fab79300544a62ee2606bd4055cdb81763fd4`.
CI also builds the ESP32-C6 firmware using the pinned ESP-IDF image.

## Receive diagnostics

`GET /api/radio` and the start page show radio readiness, the CC1101 version,
valid/invalid frame counts and receive errors. The latest CRC-valid frame is
retained even if its application payload is unknown. The latest decoded radiator
report is separate: it includes both addresses, measured temperature, active
target, raw mode and elapsed time since reception. Temperatures marked unavailable
are JSON `null`. These are last received observations, not pairing or reachability
claims; only one latest report is kept at this stage.

The receiver uses 868.3 MHz, 2-FSK, 1000/27 kbit/s, 17 kHz deviation and
101.6 kHz receive bandwidth. These are initial receive assumptions from the public
specification, not a hardware-validated profile. CC1101's 30/32 sync detector matches
the repeated D391 word, with preamble-quality gating disabled because the
preamble length is not established. NRZ, no whitening and no hardware CRC filtering are used.
Infinite packet mode preserves the length byte and both CRCs for software validation;
the radio task polls the FIFO every tick (1 ms). It uses stable byte counts and
keeps one byte buffered during reception, following the
[CC1101 FIFO errata](https://www.ti.com/lit/er/swrz020e/swrz020e.pdf).
RX stops before the final drain, then flushes/rearms after a frame, overflow or
100 ms incomplete-frame timeout. There is one bounded frame
buffer, no receive history and no radio work in HTTP handlers.

### First board test

1. Confirm the PCB wiring against [hardware](../docs/hardware.md), attach the
   antenna and flash this build. Open the serial monitor and the HTTP page.
2. Check `ready: true`, `error: 0` and a recognized CC1101 version. If initialization
   fails, check the board revision, power and wiring before changing the profile.
3. Leave the existing Touch E3/radiator system operating. Wait for normal traffic;
   check that valid-frame counts rise and the report shows plausible measured and
   target temperatures with the expected raw mode.
4. Change a target on the existing controller and watch subsequent radiator reports.
   The report age should reset on reception and continue increasing between reports.
   An unfamiliar frame can appear in diagnostics without replacing the latest report.

Software tests and the firmware build do not establish RF reception. No physical
board test has been performed. Pairing and live commands are implemented in software; neither has been
demonstrated on physical hardware.

Successful firmware CI runs publish a `cleverbox-esp32c6` artifact containing the
application, bootloader, partition table, flash arguments and resolved dependency
lock. After extracting, flash from its `build` directory with
`python -m esptool --chip esp32c6 --port PORT write_flash @flash_args`.

## Pair one radiator

Attach the antenna and complete the receive check first. Disconnect the existing
Touch E3 for the inclusion test, and put just one radiator into `rF.bi` mode.
Inclusion may reset that radiator's stored settings, as described in the external
[protocol reference](https://github.com/hemphen/clevertouch-protocol/blob/b70fab79300544a62ee2606bd4055cdb81763fd4/docs/radiator-rf/specification.md#inclusion--pairing).

Enter an eight-digit controller source and discovery destination, with matching
F1 or D0 family bytes. Choose a controller identity distinct from the old Touch E3
and the destination. The discovery destination is not the installed radiator ID.
This first implementation takes one destination explicitly; it does not guess an
enumeration range. Start pairing sends one offer, accepts an addressed inclusion
reply, then binds to the radiator's returned address. A matching inclusion ack
completes the exchange and saves both addresses in NVS.

Prototype policies: one offer and one bind, five seconds to wait for each reply,
and a one-second delay before bind. No automatic retries. TX uses the existing
2-FSK profile, a 32-bit AA preamble, repeated D391 sync, 0 dBm output and fixed
packet length, including the codec's length byte and both CRCs. These are explicit
initial TX assumptions, not a verified transmit profile. Reception is restored
after every TX attempt, including errors. Radio timing and NVS writes run in the
radio task, outside the HTTP handlers.

`POST /api/pairing` accepts JSON actions:

- `{"action":"start","source":"F1010203","destination":"F1100001"}`
  (synthetic source; select your own controller identity).
- `{"action":"cancel"}` stops the remaining steps. An already transmitted bind
  cannot be undone by cancelling.
- `{"action":"save"}` retries a failed NVS write after inclusion completed,
  without transmitting any RF frames again.

202 means the action was queued. Read `pairing` in `/api/radio` for its phase,
error code, controller/radiator addresses and `saved` flag. 400 means invalid
input; 409 means unavailable/busy/already saved or blocked storage. The single
saved radiator cannot be overwritten by starting another attempt. Removal and
multiple-radiator handling come later. Unknown/corrupt NVS records are reported
and retained; they never silently start a new inclusion.

A completed/saved inclusion handshake does not confirm an applied target or
working thermostat control. Restart loads the saved addresses and starts RX only;
it sends no offer, bind, poll or settings change. In-progress attempts are not
resumed. Ordinary firmware updates keep the NVS partition.

### Pairing acceptance on hardware (pending)

1. Start inclusion through the page and observe the reply, bind and ack phases.
   Check `saved: true` and the radiator address returned in the reply.
2. Reboot. Confirm the saved addresses remain and that no inclusion frames are
   transmitted at boot. The saved record is not a reachability indication.
3. Try cancellation during the bind delay and a timeout with no radiator in
   inclusion mode; neither should save a new pairing. Retry is explicit.

Host tests cover the exchange, address/CRC matching, timeout, cancellation and TX
failure. Tests at a fake NVS boundary cover fresh loading, version rejection and
write/commit failure. Physical radio inclusion, flash persistence across an actual
power cycle and firmware upgrades still need the board test.

## Live target/mode commands

With a saved pairing, use **Send to paired radiator** to queue an immediate
Comfort, Off, Frost or Eco command. The template field remains required for this
bench version: supply a CRC-valid 59-byte controller-state frame previously
received for this same radiator. Its destination must equal the saved radiator
address; the old controller source is replaced by Cleverbox's saved address.
Do not use another radiator's template. Undocumented configuration fields remain
opaque rather than being populated with invented defaults. Templates and pending
commands are held in RAM only; reboot never replays a requested setting.

The browser supplies its current local clock (seconds since midnight). After an
initial poll, the worker waits for a new radiator report addressed to the saved
controller. Each state transmission refreshes the temperature echo (including
unavailable), copies that report's raw E1 values, zeros DA as in the documented
controller states, updates the clock by elapsed monotonic time, sets target/mode
and recalculates both CRCs. Other fields are preserved from the template. Echoing
E1 and preserving the opaque controller fields are prototype assumptions; this
implements no new thermostat/PID loop and does not prove continued heating control.
Touch E3 is not contacted during this exchange.

`POST /api/command` takes JSON:

```text
{"state":"<same-radiator controller-state hex>","mode":0,"centi_c":2050,"clock_seconds":43200}
```

202 means queued; 400 means invalid data/template; 409 means busy, radio
unavailable or no saved pairing. HTTP and future Matter command handling share
the portable command state machine. Target limits/quantization are the same as
for previews. Off transmits raw 320 and is shown as a requested target of 0 °C.

`/api/radio` separates `command` (requested target/mode, phase, state-TX attempt
count, error and last matching report age) from `observed` (latest paired-radiator
temperature/target/mode, report age and stale flag). A transmitted state or short
ack does not count as applied. Only a later CRC-valid radiator report with the
right source/destination and matching encoded target/mode sets `matched`. That
means those settings were observed; it does not prove causality or heating output.
A later differing report remains visible separately; `matched` is a historical
result, with its age, not a permanent statement of current settings.

Prototype policy: wait at most 150 seconds for each report, allowing for the
roughly 115-second periodic exchange in the reference example. At most two state
transmissions are attempted. The second requires a new mismatching radiator
report; there are no blind timed retransmissions. After timeout or TX error,
there are no further automatic sends. A later matching report can still reconcile
an attempted state, without restarting the command. Reports for other radiators,
old pre-command observations and short acknowledgements cannot complete it.
The stale flag uses a five-minute UI threshold, not a claimed protocol heartbeat.
Commands are not persisted, scheduled or resumed after restart.

### Live-control board acceptance (pending)

1. Before disconnecting Touch E3, obtain a controller-state template for the test
   radiator. Keep private frame data outside the repository. Complete pairing and
   disconnect Touch E3 for the live command test.
2. Send a changed target with the template. Observe poll → new report → state TX →
   readback, and check the radiator-reported target/mode. Check Comfort, Eco, Frost
   and Off individually; independently observe actual heating behaviour.
3. Confirm a nonresponding radiator reaches timeout and old observations age into
   stale. Restore reception and check that late reports update observed values
   without restarting timed-out sends.
4. Reboot with a command pending: pairing must remain, command phase must be idle,
   and no target/mode command may be sent automatically. Repeat with HA/Wi-Fi absent;
   continued thermostat behaviour must be observed rather than assumed.

Host command tests cover fresh fields/CRCs, clock rollover, addressing, unsupported
modes, quantization, ACK rejection, bounded retries, timeout, TX errors and late
readback. These tests do not establish RF acceptance or thermostat behaviour.
