# Hardware

## Fixed target and parts

| Item | Purpose |
| --- | --- |
| Elecram Sub-GHz 855–925 MHz Bridge, ESP32-C6 + CC1101 | Only supported controller board |
| Matching 868 MHz antenna with the board's connector | Radiator radio |
| Matching Elecram enclosure and fixings | Finished appliance |
| 5 V USB-C supply and suitable cable | Operating power |
| USB data cable and development computer | Flashing and serial diagnostics |
| Purmo/LVI radiator supported by the reference protocol | Initial physical target |
| Home Assistant with MQTT integration and a local broker | Controller for MQTT/discovery acceptance testing |

Manufacturer: [board and accessories](https://elecram.com/products/esp32-c6-wifi-zigbee-to-855-925mhz-wireless).
The quoted delivered price for board, antenna and enclosure was 621 SEK;
this is planning context, not a maintained price or a complete power-supply quote.

A Touch E3 and SDR may be useful bench tools but are not runtime dependencies.
The board has no built-in Ethernet or PoE. A PoE-to-USB power splitter would
supply power only, not wired networking.

## Before the first firmware build

Record the received PCB revision and the manufacturer's matching schematic or
firmware revision. Populate the board configuration from those sources:

- CC1101 SPI SCLK, MOSI, MISO and chip-select.
- CC1101 GDO interrupt/data connections used by the implementation.
- User button and LED pins and polarity.
- USB flashing interface, reset and boot procedure.
- Actual flash size and partition constraints before the separate OTA milestone.

Pin assignments are intentionally not guessed here. Check the original
[manufacturer-linked firmware](https://github.com/IMMRMKW/ramses_esp) against the
received board. It is a wiring/driver reference, not the radiator application.

Fit the antenna before transmission. Use the public RF specification for initial
radio parameters; keep the resulting CC1101 register profile in the firmware's
board/radio configuration with its implementation assumptions.

## Receive prototype wiring

The radio adapter uses the manufacturer's
[`sdkconfig.defaults` at 301bbbc](https://github.com/IMMRMKW/ramses_esp/blob/301bbbc4fe46b9c7e35e681aebc2a785cea89e76/sdkconfig.defaults):

| CC1101 signal | ESP32-C6 GPIO |
| --- | ---: |
| SCLK | 6 |
| MOSI | 7 |
| MISO | 2 |
| CSn | 18 |
| GDO0 | 15 |
| GDO2 | 14 |

The received PCB revision is still unverified: compare it with the manufacturer
reference before flashing. Reception polls the FIFO; short-frame transmit completion uses GDO2.
Runtime BOOT recovery uses GPIO9, active-low with pull-up. LED behavior is not implemented.

RadioLib 7.3.0 is pinned by commit in the component manifest. The small HAL uses
ESP-IDF's public GPIO/SPI drivers because RadioLib's bundled ESP-IDF example HAL
supports the original ESP32 only. `/api/radio` reports initialization status and
CC1101 version; radio initialization failure leaves HTTP available.

## BOOT / EN reference and physical acceptance (pending)

The manufacturer-linked IMMRMKW/ramses_esp revision
[`301bbbc4fe46b9c7e35e681aebc2a785cea89e76`](https://github.com/IMMRMKW/ramses_esp/tree/301bbbc4fe46b9c7e35e681aebc2a785cea89e76)
identifies FUNC as GPIO9 in `components/ramses-buttons/include/ramses_buttons.h`
and `Kconfig.projbuild`; `ramses_buttons.c` enables its pull-up and reports levels.
`main/host.c` restarts on release after its configured 15-second hold
(`sdkconfig.defaults`). Its separate five-second Zigbee reset/re-pair gesture is
not Wi-Fi recovery. Cleverbox reuses only the verified mapping and deliberate
hold/release interaction, with its own non-destructive runtime five-second policy.

EN is hardware reset, not an application GPIO. GPIO9 is a boot-strapping pin:
**do not hold BOOT across EN/power-on** for recovery; that can enter ROM download
mode. No EN-versus-power-removal detection is promised. Supplied-enclosure BOOT
access is **unverified**; opening the enclosure or a verified access hole may be
necessary. EN plus the startup AP window remains available without serial.

Hardware checklist (software tests do not establish these):

- Record PCB revision; verify GPIO9 is high when released and low when pressed,
  with pull-up, and check normal boot with BOOT released.
- After normal boot/release, hold BOOT for at least five seconds and release:
  join **Cleverbox**, WPA2 password **cleverbox**, open **http://192.168.4.1/**.
  Check a short press does nothing and long/stuck-low holds do not repeat.
- While STA/MQTT are connected, check recovery retains STA operation and AP
  access beyond 60 seconds and through DHCP renewal. Check Finish recovery and
  successful explicit save; reconnect alone must not release the latch.
- Press EN and separately power-cycle with BOOT released: setup survives,
  recovery latch clears, startup AP uses the same credentials and no pairing or
  heating command is sent. Never claim boot-time held-low tests prove normal boot.
- Inspect actual enclosure access before suggesting a hole location. Verify BOOT
  can be operated safely; otherwise use EN and the startup AP window.
- Complete first setup and recovery on a phone without serial, including manual
  URL entry, remaining connected to an AP without internet, AP/STA channel changes,
  wrong credentials and absent-router recovery. Check confirmed Wi-Fi-only forget
  preserves radiators/templates, MQTT, timezone and discovery cleanup.
- With real radiators, verify admitted work continues through runtime recovery
  and no recovery action itself sends RF. Record Wi-Fi/flash/RF results separately.
