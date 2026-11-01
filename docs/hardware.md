# Hardware

## Parts

| Item | Use |
| --- | --- |
| [Elecram Sub-GHz 855–925 MHz Bridge](https://elecram.com/products/esp32-c6-wifi-zigbee-to-855-925mhz-wireless) (ESP32-C6 + CC1101) | The only supported board |
| 868 MHz antenna for the board's connector | Radio |
| Elecram enclosure | Housing |
| 5 V USB-C supply | Power |
| USB data cable | Flashing and serial console (USB Serial/JTAG) |
| Purmo/LVI radiator supported by the protocol reference | Target device |
| MQTT broker and Home Assistant | For MQTT and discovery tests |

A Touch E3 controller and an SDR are useful on the bench but not needed in
operation. The board has no Ethernet.

## Wiring

From the manufacturer-linked firmware
[IMMRMKW/ramses_esp at 301bbbc](https://github.com/IMMRMKW/ramses_esp/blob/301bbbc4fe46b9c7e35e681aebc2a785cea89e76/sdkconfig.defaults).
Not yet checked against a received board; record the PCB revision before
flashing.

| Signal | ESP32-C6 GPIO |
| --- | --- |
| CC1101 SCLK | 6 |
| CC1101 MOSI | 7 |
| CC1101 MISO | 2 |
| CC1101 CSn | 18 |
| CC1101 GDO0 | 15 |
| CC1101 GDO2 | 14 (transmit completion) |
| BOOT button | 9 (active low, pull-up; strapping pin) |

SPI runs at 2 MHz on SPI2 without DMA. Reception polls the FIFO; no radio
interrupts are used. The LED is not used.

## RF profile

Taken from the protocol reference. Not yet verified on hardware.

| Setting | Value |
| --- | --- |
| Frequency | 868.3 MHz |
| Modulation | 2-FSK, 17 kHz deviation |
| Bit rate | 1000/27 kbit/s (≈ 37.04 kbit/s) |
| Receive bandwidth | 101.6 kHz |
| Sync | `D391` repeated, 30 of 32 bits must match |
| Preamble | 32 bits on transmit; preamble quality not checked on receive |
| Coding | NRZ, no whitening, no hardware CRC or address filtering |
| Packet mode | Infinite on receive (length and CRCs checked in software); fixed length on transmit |
| Transmit power | 0 dBm |

## Flash layout

ESP-IDF's single factory app layout (`CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE`):
NVS at `0x9000` (24 KiB), PHY data at `0xf000`, a 1500 KiB app at `0x10000`.
It fits in 2 MiB. There are no OTA partitions. Confirm the board's flash size
before planning OTA. If you build with an existing `sdkconfig`, set the same
option there.

## Building and flashing

With ESP-IDF 5.5.1:

```sh
cd firmware
idf.py set-target esp32c6
idf.py build
idf.py -p PORT flash monitor
```

Or download the `cleverbox-esp32c6` artifact from a CI run, extract it and
flash from its `build` directory:

```sh
python -m esptool --chip esp32c6 --port PORT write_flash @flash_args
```

Flashing a new build keeps NVS (pairings and settings).

**Do not hold BOOT while resetting or powering on.** GPIO9 is a strapping pin
and the chip may enter ROM download mode.

## Bring-up checklist

Run the stages in order. Record results per stage, separately from software
test results. Keep household addresses and captures out of the
repository.

### 1. Board and radio

- [ ] Record the PCB revision and compare the wiring with the manufacturer reference.
- [ ] Confirm the flash size.
- [ ] Fit the antenna.
- [ ] Flash and open the page. `GET /api/radio` shows `ready: true`, `error: 0` and a CC1101 version.

### 2. Reception

Leave the existing Touch E3 system running.

- [ ] `valid_frames` increases.
- [ ] `report` shows plausible temperature, target and mode.
- [ ] After changing a target on the Touch E3, the next report shows it and `report.age_ms` resets.

Devices without a saved RF mode default to **listen-only**. Use **Passive
commissioning** on the local page to inspect reports from any controller.
Receiving and selecting read-only monitoring never sends RF.
An SDR with the [reference decoder](https://github.com/hemphen/clevertouch-radio)
provides independent on-air verification; software counters alone cannot prove
that the device is silent.

- [ ] Independently capture RF through boot, sustained receive, HTTP/MQTT command rejection, network/broker reconnects and reboot: no TX in listen-only.
- [ ] Identify several radiator RF addresses reporting to Touch E3 or other controllers without pairing them.
- [ ] Verify temperature, target, mode, destination, monotonic age and stale indication; unavailable values stay unavailable.
- [ ] Mode survives reboot. Corrupt/unreadable mode reports an error and closes RF without erasure.
- [ ] During sustained reception, measure heap/stack high-water marks and HTTP/MQTT responsiveness; exercise cache bounds and eviction indicators.

### 3. Network and recovery

Software tests exercise the production policy, ESP adapter at a fake driver
boundary, and HTTP admission using real loopback sockets and fake netifs. None
proves actual Wi-Fi or ESP/lwIP interface routing. All checks below remain pending.

- [ ] Fresh device with absent Wi-Fi key: `Cleverbox` AP (password `cleverbox`) loads http://192.168.4.1/ on a phone, without serial.
- [ ] Home Wi-Fi setup succeeds; the AP closes 60 s after successful save. Move the phone to the home LAN and open the station IP.
- [ ] Configured power-on and EN reset: STA reconnects and the AP is never advertised, including during startup.
- [ ] Router off, IP loss, failed reconnect batches and router return: AP remains absent; STA reconnects at the documented bounded pace.
- [ ] Corrupt/unreadable Wi-Fi record: no automatic AP or erasure; BOOT still opens diagnostics and Wi-Fi recovery. Whole-partition NVS failure must not erase flash or prevent BOOT access; writes can remain unavailable.
- [ ] Wrong replacement password and storage failures keep an already authorised AP open, including after the old STA reconnects. No new AP opens for a failed station-only configuration attempt.
- [ ] GPIO9 reads high released and low pressed. Hold BOOT 5 s and release while connected and while offline: AP opens, station/admitted commands remain intact. Short press and bounce do nothing.
- [ ] **Finish recovery** warns before submission and closes AP after 60 s both with and without STA, also while persistence is uncertain. BOOT can reopen it without serial.
- [ ] AP and STA active together: AP permits diagnostics and Wi-Fi setup only; pairing, commands, removal, time and MQTT changes return `403`. Those changes work on STA. Check an AP client targeting the station IP, changed DHCP addresses, overlapping subnets, IPv6 and spoofed Host/Origin/forwarding headers. No denied request queues work or transmits RF.
- [ ] Physical `clock_seconds` presence returns `400`; normal commands require SNTP or manual device time on STA.
- [ ] **Forget home Wi-Fi only** retains AP access and all radiators, identities, names, MQTT credentials, timezone and discovery cleanup, including across power cycles.
- [ ] EN and power cycles retain settings and send no RF. BOOT/recovery and reconnection neither initiate pairing nor replay commands; an already admitted command continues once only.
- [ ] Check BOOT accessibility through the supplied enclosure. EN is reset only. Do not hold BOOT during reset or power-on: verify GPIO9 strapping behaviour before proposing an access hole.

### 4. Time

- [ ] SNTP syncs after Wi-Fi connects (`source: network`).
- [ ] With internet blocked, manual UTC works and HTTP and RF keep working.
- [ ] The configured time zone and DST show correctly.

### 5. Pairing and control

Disconnect the Touch E3, explicitly enable control on STA, then put one
radiator in pairing mode (`rF.bi`). No template is required. Pairing
can reset the radiator's stored settings.

- [ ] Pairing goes through `offer`, `waiting_reply`, `bind`, `waiting_ack`, `complete`, and `saved: true`.
- [ ] Pairing with no radiator in pairing mode times out and saves nothing.
- [ ] Cancelling during the bind delay saves nothing.
- [ ] With no learned or saved template, send Comfort, Off, Frost and Eco. Each reaches `matched`, and the radiator display agrees.
- [ ] Heating behaviour matches the setting (observe the radiator, not only reports).
- [ ] With the radiator unpowered, a command reaches `timeout`; a later matching report changes it to `matched` without new transmissions.
- [ ] Listen-only request during pairing/command work is refused clearly; after bounded completion retry it and independently verify silence. Control enable alone sends nothing.
- [ ] Touch E3 disconnected → explicit enable → pair → set time → command works with real equipment; the transmitted controller source is Cleverbox’s paired identity.
- [ ] Reboot with a command pending: the pairing remains, the command is not resumed and nothing is sent at boot.

Generated-profile acceptance remains unproven:

- [ ] Pair under a new explicit controller identity and capture on-air bytes with
  an independent receiver. Compare addresses, all fixed records, report echoes,
  `5B=0078`, local time and both CRCs against the generated output.
- [ ] Confirm display and matching later report separately from local TX/ACK;
  observe physical heating over time for all four modes.
- [ ] Change a setting locally at the radiator, then answer its local-change-marked
  report; check the profile's explicit `97` policy with capture and readback.
- [ ] Check persisted current-format identities/names and no RF at boot/enable/recovery.

Record contradictions in clevertouch-protocol. Host, simulator and HA tests do
not prove identity acceptance, wire acceptance or physical regulation.

### 6. Two radiators and overlapping traffic

- [ ] Dump CC1101 registers before receive, during transmit and after receive restarts; compare with the [RF profile](#rf-profile).
- [ ] Commands to two radiators run independently; a silent radiator does not block the other.
- [ ] Measure valid and missing reports, time from ready to transmit and attempt counts, first with no overlap, then with commands queued while the other radiator's report is starting, midway and ending. Compare with the baseline.
- [ ] Malformed bursts and sustained traffic: reception recovers, HTTP and MQTT stay responsive, expired work is never sent.
- [ ] Transmit failure: reception restarts and later frames are received.

Expect some loss: a frame can start after the last FIFO check, and reception
stops while a frame is being sent.

### 7. MQTT and Home Assistant

- [ ] Broker connection with authentication over Wi-Fi.
- [ ] `offline` is published by the last will after a power cut and by a router outage.
- [ ] Discovery devices appear; rename and removal update Home Assistant.
- [ ] Apply from Home Assistant reaches `matched`.
- [ ] An admitted command completes while the broker or Wi-Fi is down.

Passive monitoring checks (software/HA success is not physical heating evidence):

- [ ] With Touch E3 active and Cleverbox listen-only, select several full RF addresses and verify their HA devices without pairing, including different/changing destination controllers.
- [ ] Independently capture RF during select, rename, removal, discovery enable/disable, HA birth, broker reconnect and bridge restart: zero transmissions.
- [ ] Compare measured temperature, observed target/mode, unavailable markers and freshness with genuine reports and radiator/Touch E3 displays. Verify five-minute expiry and 15-second publication-loss expiry separately.
- [ ] Power-cycle: names/selections persist, observations start unavailable, reports restore the same HA identities. Pending removal finishes without ghost entities.
- [ ] Under sustained traffic beyond general cache capacity, selected devices continue updating and HTTP/MQTT remain responsive. Measure heap and task stack high-water marks.

### 8. Endurance

- [ ] Run for several days with all radiators. Record task stack and heap high-water marks.
- [ ] Settings survive repeated power cycles.
- [ ] Heating continues with Home Assistant, the network and the Touch E3 absent.

## Radio fault acceptance

All radio recovery evidence so far is software fault injection. Record real
hardware results separately. Keep the antenna fitted; do not hot-plug radio,
power or SPI wires while powered. Use a controlled test firmware hook that
returns a driver error, safe test fixtures, or faults already present on the
bench. Power down before changing wiring. A software-injected error alone does
not establish electrical fault recovery.

- [ ] Record real driver errors and bus/radio failure stages without resets or aborts. Restore a safe fixture and verify RX after automatic recovery and after exhausted-budget STA retry. Missing hardware must stop retrying after four attempts.
- [ ] Induce bounded RX-restoration failure safely. Verify the complete RF/register profile after reinitialization, reception of genuine reports and no permanently busy radiator reservations.
- [ ] Test TX and post-TX restoration errors; compare independent packet capture with uncertain delivery diagnostics. No automatic probe, pairing or replay may appear.
- [ ] Independently capture RF in listen-only throughout initial failure, automatic recovery, exhaustion, repeated STA retries, reconnect and reboot: exact on-air silence.
- [ ] Run sustained multi-radiator reception alongside HTTP, MQTT and HA traffic and repeated independent faults. Measure packet loss, network latency and recovery timing.
- [ ] Check paired and passive HA readings unavailable on radio failure, increasing original report age, stable identities, old expired readings still unavailable after recovery and genuine new reports restoring freshness.
- [ ] Measure heap/free-block minima, radio/HTTP/MQTT stack high-water marks and watchdog behavior during setup failures, driver timeouts and sustained RX/TX traffic. Confirm the retained SPI transaction buffers are never reused while in flight.
- [ ] Check no replay or pairing after recovery, HA/bridge/broker restart. Pairings, IDs, passive selections, RF mode and unrelated NVS settings survive.

A silently wrong SPI response or failed RF front end without an observable error
is not guaranteed detectable. Driver/internal assertion deadlocks are not
recoverable by this owner policy; see [adapter limits](architecture.md#adapter-bounds-and-limits).

## Transmit completion acceptance

Software polling has not been tested on a board. Use the same RF profile,
antenna, radiators and workload for baseline and candidate comparisons.

- [ ] Record PCB revision and verify GPIO14/GDO2 and GPIO15/GDO0 against the manufacturer source. Scope GDO2 sync assertion and packet-end deassertion alongside SPI STX, TXBYTES/MARCSTATE sampling and RX re-entry. Include a pulse entirely between task polls.
- [ ] Independently capture offer/bind/poll/state packets; compare exact envelopes/CRCs and TX count with original request results. Local completion is not acceptance; verify radiator readback and display separately.
- [ ] Measure TX-to-RX turnaround and missed incoming frames before, during and after TX. Dump fixed/infinite packet mode, GDO mappings, threshold and the full RF profile after success, timeout and recovery. No reception gain is assumed.
- [ ] Safely exercise stuck/absent GDO, failed start, underflow, SPI/finalizer/restoration failures and exhausted retry, using controlled fixtures or diagnostic hooks. Compare actual RF with confirmed-pre-STX versus uncertain-delivery results. Never hot-plug powered wires.
- [ ] Exercise multiple radiators (including a silent one), ready/queued deadline expiry, queued removal/rename writes and pair cancel during TX. Check fairness, single physical TX, exact attempt counts, original HTTP/MQTT request attribution and honest persistence outcomes.
- [ ] Measure HTTP/MQTT latency during TX, queue backlog and simultaneous independent NVS settings writes. Record watchdog behavior, minimum heap/largest free block, and radio/HTTP/MQTT stack high-water marks. No CPU, scheduling or heap improvement is claimed from host tests.
- [ ] Independently verify exact RF silence in listen-only through faults, exhaustion/retry, reconnect, HA birth and reboot. Verify failed RF work is never replayed; accepted local writes retain their outcomes and ordering.
