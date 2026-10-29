# Libraries

Use C/C++ with ESP-IDF and RadioLib. Use ESP-IDF's HTTP server and RadioLib
transport. MQTT/HA discovery follows setup and home-network/time milestones;
ESP-Matter is an optional later dependency.

| Dependency | Use |
| --- | --- |
| [ESP-IDF](https://github.com/espressif/esp-idf) | FreeRTOS, SPI/GPIO, Wi-Fi, HTTP server, NVS, SNTP/system time and timers |
| Optional later: [ESP-Matter](https://github.com/espressif/esp-matter) | Matter commissioning, bridge lifecycle and thermostat endpoints |
| [RadioLib](https://github.com/jgromes/RadioLib) | CC1101 driver through its native ESP-IDF integration |

The stage-1 HTTP draft targets ESP-IDF v5.5.1. RadioLib 7.3.0 is pinned to commit
`2a2f6e0edf5a527cd83d2a04a7e2eb5d220329cd` in the component manifest.
Only the CC1101 driver and core support are enabled. Pin ESP-Matter when added.
The RX and TX profiles still need physical board acceptance.

## Application components

- **radiator_protocol:** frame encoding/decoding, CRCs, inclusion payloads,
  temperature conversion and mode values. Plain C interfaces, independent of
  hardware and buildable in host tests.
- **radio_transport:** Elecram pin configuration, RadioLib integration, interrupts,
  queues and timed exchanges.
- **device_store:** bounded versioned NVS registry, legacy migration, names, pairings and saved controller state.
- **network_setup:** ESP-IDF Wi-Fi task/event adapter, portable bounded setup/reconnect
  policy and separate Wi-Fi NVS key. Uses no provisioning framework. See [networking](networking.md).
- **device_time:** shared UTC/provenance and validated POSIX timezone logic, separate
  configuration-only NVS key, ESP-IDF SNTP adapter and explicit manual UTC setup.
  No timezone database or third-party time dependency. See [time](time.md).
- **matter_bridge (optional later):** thermostat attributes and commands mapped to radiator operations.

Keep one codec and one set of protocol values. Reuse these components throughout
the application. Follow the external protocol specification rather than copying
its documentation here.

Home Assistant executes weekly schedules by sending target changes. Cleverbox
does not store or execute schedules; this is outside its scope. Cleverbox
reports radiator state separately from requested settings. RF Auto Comfort/Eco
codes are schedule phases, not a direct mapping to Matter Auto mode.

HTTP, the host-tested codec/pairing/command logic, RadioLib RX/TX adapter and
multi-radiator NVS registry with independent per-radiator delivery and serialized CC1101 transmissions are present.
ESP-Matter is not imported. The host-only platform/HTTP/NVS adapters are described
in [simulation](simulation.md); they are not part of the physical firmware.
