# Firmware

This directory will become the ESP-IDF / ESP-Matter project. It deliberately
contains no nonfunctional build scaffold yet.

Create the top-level CMakeLists.txt, sdkconfig.defaults, dependency manifest and
partition table with the first buildable change. Target esp32c6 only. Keep SDK
version selection and build/flash/monitor instructions here.

Introduce these directories as their implementations arrive:

| Path | Responsibility |
| --- | --- |
| main/ | Application startup, board configuration and user controls |
| components/radiator_rf/ | Pure frame codec plus CC1101 transport and inclusion/session handling |
| components/radiators/ | Per-radiator state, persistence and command reconciliation |
| components/matter_bridge/ | Matter endpoints and translation to radiator operations |

These are small internal components, not public extension APIs. Do not create a
generic hardware abstraction or a transport plugin system. Keep pure codec tests
beside the component; add hardware acceptance instructions to the implementation
plan when an executable build exists.
