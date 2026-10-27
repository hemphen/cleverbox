# Firmware

The receive prototype uses ESP-IDF **v5.5.1**, its HTTP server and the portable C protocol
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
**It receives frames but does not transmit, discover or pair radiators yet.**
Every command result is labelled preview. Frame bytes begin at the length byte;
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
Bind uses the responding radiator's address. Automatic discovery and pairing
state are not implemented in this draft.

Target changes require a complete CRC-valid 59-byte controller-state frame:

```text
{"command":"target","state":"<controller-state hex>","mode":0,"centi_c":2050}
```

Modes: 0 Comfort, 1 Off, 2 Frost, 3 Eco. Non-Off targets are 5–30 °C
(stage-1 application limits), passed as integer hundredths of a degree.
Off uses the reference's raw target 320. The codec updates tag 94 and both CRCs,
preserving addresses, opaque fields and clock bytes. This is a frame-editing
primitive; fresh clock/state handling must be added before live transmission.
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
board test has been performed. Pairing and live commands are the next milestone.

Successful firmware CI runs publish a `cleverbox-esp32c6` artifact containing the
application, bootloader, partition table, flash arguments and resolved dependency
lock. After extracting, flash from its `build` directory with
`python -m esptool --chip esp32c6 --port PORT write_flash @flash_args`.
