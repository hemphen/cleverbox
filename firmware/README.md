# Firmware

The ESP-IDF / ESP-Matter project will live here, targeting esp32c6.
See [libraries and components](../docs/libraries.md).

Create CMakeLists.txt, sdkconfig.defaults, the dependency manifest and partition
table with the first buildable change. Pin compatible dependencies and document
build, flash and monitor commands here.

| Path | Responsibility |
| --- | --- |
| main/ | Startup, board configuration and user controls |
| components/radiator_protocol/ | Hardware-independent frame codec and protocol values |
| components/radio_transport/ | Elecram/RadioLib driver and exchange handling |
| components/device_store/ | Pairing storage and stable endpoint identities |
| components/matter_bridge/ | Matter thermostat mapping |

Add directories when their implementation starts. Keep codec tests beside the
component; appliance tests cover transport, Matter mapping and persistence.
