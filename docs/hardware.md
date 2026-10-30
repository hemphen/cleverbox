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

Flashing a new build keeps NVS (pairings, templates, settings).

**Do not hold BOOT while resetting or powering on.** GPIO9 is a strapping pin
and the chip may enter ROM download mode.

## Bring-up checklist

Run the stages in order. Record results per stage, separately from software
test results. Keep household addresses, templates and captures out of the
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
- [ ] Capture a 59-byte controller-state frame (hex starting `38`) from the Touch E3 to each radiator. These are candidate templates for stage 5.

Cleverbox has no reliable capture mechanism yet. `last_frame` in
`GET /api/radio` holds only the latest CRC-valid frame of any type and address.
The next frame, such as an acknowledgement, a report or another radiator's
traffic, replaces it, often before the page refreshes every 2 s. Capture with an
external receiver, such as an SDR with the
[clevertouch-radio](https://github.com/hemphen/clevertouch-radio) decoder.
Sampling `/api/radio` in a tight loop and keeping each `last_frame` that starts
with `38` and has the radiator as destination (bytes 6–9) can also work, but it
is opportunistic.

### 3. Network and recovery

- [ ] The `Cleverbox` access point is up at boot; http://192.168.4.1/ loads on a phone.
- [ ] Home Wi-Fi setup succeeds; the access point closes 60 s after the station gets an IP.
- [ ] A wrong password reports `setup_result: failed` and keeps the previous setup.
- [ ] Router off: the access point reopens. Router on: the station reconnects.
- [ ] GPIO9 reads high when released and low when pressed.
- [ ] BOOT held 5 s and released opens the access point; a short press does nothing; the station stays connected.
- [ ] **Finish recovery** closes the access point 60 s later.
- [ ] EN and a power cycle keep all settings and send no RF.
- [ ] **Forget home Wi-Fi only** keeps radiators, templates, MQTT and time zone.
- [ ] Check whether BOOT is reachable through the enclosure.

### 4. Time

- [ ] SNTP syncs after Wi-Fi connects (`source: network`).
- [ ] With internet blocked, manual UTC works and HTTP and RF keep working.
- [ ] The configured time zone and DST show correctly.

### 5. Pairing and control

Disconnect the Touch E3. Put one radiator in pairing mode (`rF.bi`). Pairing
can reset the radiator's stored settings.

- [ ] Pairing goes through `offer`, `waiting_reply`, `bind`, `waiting_ack`, `complete`, and `saved: true`.
- [ ] Pairing with no radiator in pairing mode times out and saves nothing.
- [ ] Cancelling during the bind delay saves nothing.
- [ ] Import a template for the radiator and send Comfort, Off, Frost and Eco. Each reaches `matched`, and the radiator display agrees.
- [ ] Heating behaviour matches the setting (observe the radiator, not only reports).
- [ ] With the radiator unpowered, a command reaches `timeout`; a later matching report changes it to `matched` without new transmissions.
- [ ] Reboot with a command pending: the pairing and saved template remain, the command is not resumed and nothing is sent at boot.

Open questions to answer here, using genuine captured templates only:

- [ ] Does a template captured from the Touch E3 work after pairing under a new controller address?
- [ ] Does a saved template stop working over days?
- [ ] After a change made at the radiator itself, its next report has `DF` byte 1 bit 0 set. Does a command answering that report behave normally? The Touch E3 was never observed sending a state frame in reply to such a report.

Do not substitute values for template fields during acceptance. A `matched`
result would not show that a substitute is harmless. Building a frame without a
template is a separate protocol investigation with a known-good baseline and a
recovery procedure; its questions belong in clevertouch-protocol (see
[AGENTS.md](../AGENTS.md#protocol)).

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

### 8. Endurance

- [ ] Run for several days with all radiators. Record task stack and heap high-water marks.
- [ ] Settings survive repeated power cycles.
- [ ] Heating continues with Home Assistant, the network and the Touch E3 absent.
