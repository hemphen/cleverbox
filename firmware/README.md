# Firmware

Stage 1 uses ESP-IDF **v5.5.1**, its HTTP server and the portable C protocol
component. No Matter controller or CC1101 wiring is needed to exercise the page.

```sh
# With ESP-IDF v5.5.1 installed and export.sh loaded:
cd firmware
idf.py set-target esp32c6
idf.py build
idf.py -p PORT flash monitor
```

Join the **Cleverbox** Wi-Fi network using the temporary password printed on
the serial console. Open **http://192.168.4.1**. The password changes each boot.
This bench build starts an access point; home-network provisioning comes later.

The start page prepares pairing offer/bind, poll and target/mode frames.
**It does not transmit, receive, discover or pair radiators yet.**
Every result is labelled preview. Frame bytes begin at the length byte;
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

Connect RadioLib through a separate radio_transport component next. Replace
preview-only handling with explicit transmit/receive state, bounded inclusion
and radiator readback. Keep HTTP handlers free of radio timing loops.
