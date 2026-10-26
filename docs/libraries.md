# Libraries

Use C/C++ with ESP-IDF, ESP-Matter and RadioLib. Start with ESP-IDF's HTTP
server; add RadioLib transport next and Matter over Wi-Fi afterward.

| Dependency | Use |
| --- | --- |
| [ESP-IDF](https://github.com/espressif/esp-idf) | FreeRTOS, SPI/GPIO, Wi-Fi, HTTP server, NVS and timers |
| [ESP-Matter](https://github.com/espressif/esp-matter) | Matter commissioning, bridge lifecycle and thermostat endpoints |
| [RadioLib](https://github.com/jgromes/RadioLib) | CC1101 driver through its native ESP-IDF integration |

The stage-1 HTTP draft targets ESP-IDF v5.5.1. Add and pin RadioLib and
ESP-Matter when their components are implemented. Configure RadioLib
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

The HTTP draft and host-tested codec are present. RadioLib and ESP-Matter
are selected dependencies, not yet imported.
