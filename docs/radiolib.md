# RadioLib

Cleverbox uses RadioLib **7.8.1**, commit
`2325a38a89dd0f0e402c19f67fe910111da1e246`, with ESP-IDF 5.5.1. The component
manifest and `firmware/dependencies.lock` pin the commit, and the firmware
checks `RADIOLIB_VERSION` at compile time. After changing the pin, run
`idf.py update-dependencies`: an ordinary build can keep the locked revision.

Only the CC1101 driver is built (`firmware/CMakeLists.txt` excludes the rest).

## What Cleverbox uses

- **HAL**: RadioLib's bundled ESP-IDF HAL (`EspHal`) on SPI2 at 2 MHz with
  software chip select. It needs `CONFIG_FREERTOS_HZ=1000`, which
  `sdkconfig.defaults` sets. It logs SPI errors but does not return them.
- **Setup**: `begin()` with the [RF profile](hardware.md#rf-profile), then
  explicit register writes for 30/32-bit sync detection and infinite packet
  length, and `startReceive()`.
- **Receive**: Cleverbox reads the FIFO itself (see
  [receive path](architecture.md#receive-path)); RadioLib's packet receive is
  not used.
- **Transmit**: `standby()`, `fixedPacketLengthMode(length)` and the blocking
  `transmit()`. It waits for GDO2 (GPIO14) to rise and fall, with a timeout of
  5 ms plus five times the frame's air time, then returns to IDLE. Cleverbox
  then restores infinite length and calls `startReceive()`. Frames are at most
  64 bytes, so the FIFO is never refilled.

## Tests

`make RADIOLIB_SOURCE=/path/to/checkout test-radiolib` compiles the real
RadioLib CC1101 driver against a register model (`test/radiolib`). It checks
the configured RF profile registers, that fixed-length frames of 14, 59 and 64
bytes go out unchanged, that a GDO2 that never rises or falls ends in a bounded
timeout, and that receive is restored. CI runs it against the pinned commit.
It does not test an ESP-IDF build or real hardware.
