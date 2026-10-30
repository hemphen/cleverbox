# RadioLib compatibility

Use **7.8.1**, commit `2325a38a89dd0f0e402c19f67fe910111da1e246`, with
ESP-IDF 5.5.1. Both the component manifest and generated lock pin the commit.
The firmware also checks `RADIOLIB_VERSION` at compile time. After changing a
Git pin, run `idf.py update-dependencies`: an ordinary build can retain the
previous locked revision. Check the resolved source, not just the manifest.

## Upgrade decision

Compared with 7.3.0 (`2a2f6e0edf5a527cd83d2a04a7e2eb5d220329cd`),
7.8.1 is a useful, software-compatible upgrade for our short, blocking CC1101
packet transmissions. It was the latest non-prerelease upstream release when
checked on 2026-10-30. The small timing and cleanup improvements justify the
update; improved physical reliability is not established.

[Release](https://github.com/jgromes/RadioLib/releases/tag/7.8.1) ·
[complete comparison](https://github.com/jgromes/RadioLib/compare/7.3.0...7.8.1) ·
[candidate CC1101 source](https://github.com/jgromes/RadioLib/blob/2325a38a89dd0f0e402c19f67fe910111da1e246/src/modules/CC1101/CC1101.cpp)

| Upstream change | Effect here |
| --- | --- |
| [4ca36bc04](https://github.com/jgromes/RadioLib/commit/4ca36bc04): FSTXON/MARCSTATE wait 800 → 1600 µs | Applies to every TX. The register harness accepts a synthetic 1100 µs calibration with 7.8.1 and returns TX timeout with 7.3.0. No hardware timing is inferred. |
| [4bba92f38](https://github.com/jgromes/RadioLib/commit/4bba92f38): GDO cleanup | Successful `finishTransmit()` now sets IOCFG2 to high impedance. The next short TX remaps it to sync-sent/packet-end. GDO2 remains GPIO14; no interrupt is introduced. Cleanup write errors propagate. `finishReceive()` cleanup is unused by our custom FIFO reader. |
| [dff0ee6f1](https://github.com/jgromes/RadioLib/commit/dff0ee6f1), [2ee81780a](https://github.com/jgromes/RadioLib/commit/2ee81780a), [f9fcb6c11](https://github.com/jgromes/RadioLib/commit/f9fcb6c11): streaming counters, completion mapping and FIFO-feeding timeout | No benefit for our fixed, unfiltered frames of at most 64 bytes. We never enter the refill loop. Its inner stable-count loop is still unbounded; the new outer timeout does not bound that inner loop. |
| [f2a0d1e3d](https://github.com/jgromes/RadioLib/commit/f2a0d1e3d): bandwidth comparison | Fixes selection for unsupported/intermediate requests. Our 101.6 kHz request gives identical MDMCFG4 `CA` in both versions (actual bandwidth 101.5625 kHz). No RF setting changes. |
| [510e00cfb](https://github.com/jgromes/RadioLib/commit/510e00cfb): repeated-sync API and TXOFF_RX | Unused. Keep the existing explicit 30/32 sync register setting, TXOFF_IDLE and explicit infinite-RX restoration. |
| [187ef2479](https://github.com/jgromes/RadioLib/commit/187ef2479): version `03` | Additional clone acceptance may help some boards; our board revision/chip are unknown. The positional `begin()` remains supported and uses the same explicit values. No need to change it. |

`standby()` and `startReceive()` retain their relevant behaviour: IDLE wait,
RX FIFO flush, GDO0 FIFO mapping, threshold and RX strobe. Fixed-length TX still
sends the complete envelope, including the two software CRCs, without a library
length/address byte. The blocking GDO2 start/end waits and their timeout formula
are unchanged. `finishTransmit()` additionally checks TXOFF mode, but ours stays
IDLE. Error handling still needs the caller's unconditional RX restoration.

Module register reads/writes and readback checks remain compatible. The paranoid
SPI interval arithmetic fix does not affect our default 2 ms interval. Module
copy fixes are unused (we do not copy it), and stream-status/debug/static-buffer
changes do not alter our register-mode, debug-off, dynamic-buffer configuration.
HAL additions (virtual destructor, optional pull control and AES pointer) do not
require overrides. The bundled ESP-IDF HAL is now built, but Cleverbox still
instantiates **IdfHal**, with the same SPI/GPIO/timing methods and empty inherited
`yield()`. Its SPI failures still use `ESP_ERROR_CHECK`, not recoverable library
error returns. The upgrade does not fix those limitations or our custom FIFO
handling. New LR2021/ADS-B code is explicitly excluded alongside the existing
unused drivers/protocols.

## Software validation

Baseline: current main `aecfae5094c4f3648a0c2147307ed31cf4945d2b`, including
PR #15's merge `3b576ca53c49f7d24ed30cbdfb4d2da3b26cc4a5`. No unmerged
application branch was incorporated. The outstanding CI branch was inspected;
its CI changes were already squash-merged into main.

Both versions passed `make test`, `test-http`, `test-capacity`,
`test-discovery-limit` and `test-mqtt-broker` with the same host tools,
Mosquitto 2.0.18 and Paho 2.1.0. These suites use fake RadioLib/FIFO boundaries;
they do not validate the real dependency.

The separate `test/radiolib` harness compiles **real** RadioLib CC1101, Module
and HAL code. It provides only deterministic register/strobe responses, GDO
levels and a clock; it is not an RF or chip simulator. Run it against either
exact checkout (CMake and a C++ compiler required):

```sh
make BUILD_DIR=build/rl73 RADIOLIB_SOURCE=/path/to/RadioLib-7.3.0 test-radiolib
make BUILD_DIR=build/rl78 RADIOLIB_SOURCE=/path/to/RadioLib-7.8.1 test-radiolib
```

Both pass the same golden profile, 14/59/64-byte fixed TX payload checks,
GDO2 mapping/cleanup expectations, calibration/completion timeout probes,
RX restoration and refused-register/standby error tests. Unsupported bandwidth
and 1100 µs calibration distinguish the versions. CI runs the candidate harness
against the manifest's exact commit. It does not exercise the production ESP-IDF
HAL, analog RF, real GDO timing, or large-packet fixes we do not use.

The production transport tests still exercise partial/software and FIFO-only
reception, late FIFO arrival, unstable counts, bounded sampling/recovery,
overflow, malformed frames, final-guard expiry, ready retry/readback ordering,
retained work/attempts/cursor, two-radiator progress, TX errors and RX restoration.
Removing the partial-frame handoff guard and final idle gate makes
`test_partial_and_fifo_only` fail before TX; the unchanged worker passes.
Requested/observed state, bounded retries, storage formats, network/time policy
and MQTT destruction-before-replay ordering are unchanged.

## Firmware and resources

Both exact versions compiled for ESP32-C6 with ESP-IDF **v5.5.1**, GCC
**14.2.0 / esp-14.2.0_20241119**, identical SDK configuration, project version
`radiolib-eval`, default capacity eight and the unchanged 1500 KiB app partition.
The baseline used an exact local checkout through `EXTRA_COMPONENT_DIRS` with
the component manager disabled. The candidate used its regenerated lock and
`managed_components/RadioLib`. `compile_commands.json` identified the compiled
CC1101 files; their bytes matched the corresponding exact upstream checkouts.
The local component manager required its documented `os.getppid()` fallback
because this environment lacks usable process-tree visibility. That tool-only
workaround changes no firmware/library source; CI uses the normal IDF image.

| Measured build output | 7.3.0 | 7.8.1 | Change |
| --- | ---: | ---: | ---: |
| Application `.bin` | 1,166,784 B | 1,167,408 B | +624 B |
| IDF image size, before padding | 1,166,680 B | 1,167,296 B | +616 B |
| `.data` | 14,344 B | 14,344 B | 0 |
| `.bss` | 49,840 B | 49,848 B | +8 B |
| Static data + BSS | 64,184 B | 64,192 B | +8 B |
| App partition free | 369,216 B | 368,592 B | −624 B |

Both have 24% free app space; reserved LP SRAM is unchanged at 24 B. These are
linker/image measurements, **not** runtime heap, stack high-water marks, CPU use
or network responsiveness. The small increases do not require a partition or
capacity change. Home Assistant integration is checked separately in CI; the
ordinary Build workflow does not cover it.

## Physical comparison still required

No board or radiator test was performed. Compare old and new images on the
same board and settings, with an antenna and independent packet capture:

- Record PCB revision and verify GDO0/GDO2 wiring against the pinned
  [manufacturer source](hardware.md#wiring).
- Dump RF/packet registers before TX and after infinite-RX restoration. Compare
  frequency, deviation, bandwidth, sync, preamble, CRC/filtering and power;
  account for the intentional post-TX GDO2 high-impedance change.
- Compare captured offer/bind/poll/state bytes, both software CRCs and timings.
- Exercise completion, absent/stuck GDO and TX failure/timeouts; check error
  reporting and subsequent RX/profile restoration.
- Repeat partial-frame overlaps, malformed/truncated traffic and overflow.
  Record loss, deferrals and command attempts; the final-sample collision window
  and blocking TX receive gap remain.
- Exercise two radiators, one silent, while measuring HTTP/MQTT latency and
  per-radiator progress under Wi-Fi/broker loss/reconnect. Check no replay at boot.

See the full [bring-up checklist](hardware.md#bring-up-checklist).

## Next milestone

Nonblocking **short-packet TX** is the appropriate next focused transport
milestone. The current owner still busy-polls GDO2 through an empty HAL yield;
the upgrade does not establish lower CPU use or responsiveness. Evaluate
`startTransmit()` plus owner-polled completion and a bounded deadline, keeping
all SPI access in that owner and preserving receive-first arbitration. Note that
`startTransmit()`, `standby()` and `finishTransmit()` still contain blocking
waits, so merely renaming the transmit call is not sufficient. Measure scheduling
and responsiveness on the board; receive interrupts, bounded radio recovery and
OTA remain separate work.
