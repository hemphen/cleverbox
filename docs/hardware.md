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

SPI runs at 2 MHz on SPI2 through RadioLib's ESP-IDF HAL. Reception polls the
FIFO; no radio interrupts are used. Transmission polls GDO2. The LED is not used.

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

Flashing a new build keeps NVS (pairings and settings). To start from scratch,
run `idf.py -p PORT erase-flash` before flashing; this erases all settings.

**Do not hold BOOT while resetting or powering on.** GPIO9 is a strapping pin
and the chip may enter ROM download mode.

## Bring-up checklist

Run the stages in order and record results separately from software test
results. Keep household addresses and captures out of the repository. An SDR
with the [reference decoder](https://github.com/hemphen/clevertouch-radio)
gives independent on-air verification; the device's own counters cannot prove
what was sent.

### 1. Board and radio

- [ ] Record the PCB revision, compare the wiring with the manufacturer reference and confirm the flash size.
- [ ] Fit the antenna, flash and open the page. `GET /api/radio` shows `ready: true` and a CC1101 version.
- [ ] Without the antenna or CC1101 responding, `ready` is false with an `init` error, and the page still works.

### 2. Reception

Leave the Touch E3 system running. Nothing in this stage transmits.

- [ ] `valid_frames` increases and **Heard radiators** lists the radiators with plausible temperature, target and mode.
- [ ] After a target change on the Touch E3, the next report shows it.
- [ ] Monitor a heard radiator; its record keeps updating.
- [ ] Capture RF through boot, monitoring and reconnects: nothing is sent.

### 3. Network and time

- [ ] Fresh device: the `Cleverbox` AP (password `cleverbox`) serves http://192.168.4.1/ on a phone.
- [ ] Save home Wi-Fi; the station connects and the page works on the station IP. **Close setup AP** closes it.
- [ ] Restart and router outage: the station reconnects; the AP stays closed.
- [ ] Hold BOOT for 5 s after startup, then release: the AP opens. A short press does nothing.
- [ ] SNTP syncs after Wi-Fi connects (`source: network`); manual UTC works offline; the time zone and DST show correctly.

### 4. Pairing and control

Disconnect the Touch E3 and put one radiator in pairing mode (`rF.bi`).
Pairing can reset the radiator's stored settings.

- [ ] Pairing goes through `offer`, `waiting_reply`, `bind`, `waiting_ack` and `complete`; the record survives restart.
- [ ] Pairing with no radiator in pairing mode times out and adds nothing.
- [ ] Comfort, Off, Frost and Eco each reach `matched`, and the radiator display agrees.
- [ ] Capture the generated frames on air and compare addresses, records, report echoes, `5B=0078`, local time and both CRCs with the software output.
- [ ] Heating behaviour matches the setting (observe the radiator, not only reports).
- [ ] With the radiator unpowered, a command reaches `timeout`; a later matching report changes it to `matched` without new transmissions.
- [ ] Restart with a command running: it is not resumed and nothing is sent at boot.

### 5. Two radiators

- [ ] Commands to two radiators run one after the other; a silent radiator holds the queue for 150 s, then the other runs.
- [ ] Dump CC1101 registers before receive, during transmit and after receive restarts; compare with the [RF profile](#rf-profile).
- [ ] Measure missed reports around transmissions and settings saves.

### 6. MQTT and Home Assistant

- [ ] Broker connection with authentication; `offline` appears after a power cut.
- [ ] Discovery devices appear for paired and monitored radiators; rename and removal update Home Assistant.
- [ ] Apply from Home Assistant reaches `matched`. A command already queued completes while the broker is down.

### 7. Endurance

- [ ] Run for several days with all radiators. Record task stack and heap high-water marks and any radio errors.
- [ ] Settings survive repeated power cycles.

Record protocol contradictions in clevertouch-protocol.
