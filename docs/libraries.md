# Libraries

Use C/C++ with ESP-IDF, ESP-Matter and RadioLib. Matter runs over Wi-Fi.

| Dependency | Use |
| --- | --- |
| [ESP-IDF](https://github.com/espressif/esp-idf) | FreeRTOS, SPI/GPIO, Wi-Fi, NVS and timers |
| [ESP-Matter](https://github.com/espressif/esp-matter) | Matter commissioning, bridge lifecycle and thermostat endpoints |
| [RadioLib](https://github.com/jgromes/RadioLib) | CC1101 driver through its native ESP-IDF integration |

Pin compatible versions in the first working ESP32-C6 build. Configure RadioLib
for CC1101 only and check the resulting firmware size. Verify its framing,
sync, FIFO and RX/TX controls against the radiator profile during board bring-up.

## Application components

- **radiator_protocol:** frame encoding/decoding, CRCs, inclusion payloads,
  temperature conversion and mode values. Plain C interfaces, independent of
  hardware and buildable in host tests.
- **radio_transport:** Elecram pin configuration, RadioLib integration, interrupts,
  queues and timed exchanges.
- **device_store:** NVS-backed pairing data and stable Matter endpoint identities.
- **matter_bridge:** thermostat attributes and commands mapped to radiator operations.

Keep one codec and one set of protocol values. Reuse these components throughout
the application. Follow the external protocol specification rather than copying
its documentation here.

Home Assistant executes weekly schedules by sending target changes. Cleverbox
does not store or execute schedules; this is outside its scope. Cleverbox
reports radiator state separately from requested settings. RF Auto Comfort/Eco
codes are schedule phases, not a direct mapping to Matter Auto mode.

This is the selected stack. The first firmware build will establish compatible
versions and verify operation on the Elecram.
