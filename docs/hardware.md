# Hardware

## Fixed target and parts

| Item | Purpose |
| --- | --- |
| Elecram Sub-GHz 855–925 MHz Bridge, ESP32-C6 + CC1101 | Only supported controller board |
| Matching 868 MHz antenna with the board's connector | Radiator radio |
| Matching Elecram enclosure and fixings | Finished appliance |
| 5 V USB-C supply and suitable cable | Operating power |
| USB data cable and development computer | Flashing and serial diagnostics |
| Purmo/LVI radiator supported by the reference protocol | Initial physical target |
| Home Assistant with Matter integration and a working local IPv6/multicast network | First controller for acceptance testing |

Manufacturer: [board and accessories](https://elecram.com/products/esp32-c6-wifi-zigbee-to-855-925mhz-wireless).
The quoted delivered price for board, antenna and enclosure was 621 SEK;
this is planning context, not a maintained price or a complete power-supply quote.

A Touch E3 and SDR may be useful bench tools but are not runtime dependencies.
The board has no built-in Ethernet or PoE. A PoE-to-USB power splitter would
supply power only, not wired networking.

## Before the first firmware build

Record the received PCB revision and the manufacturer's matching schematic or
firmware revision. Populate the board configuration from those sources:

- CC1101 SPI SCLK, MOSI, MISO and chip-select.
- CC1101 GDO interrupt/data connections used by the implementation.
- User button and LED pins and polarity.
- USB flashing interface, reset and boot procedure.
- Flash size and partition constraints for the selected Matter build.

Pin assignments are intentionally not guessed here. Check the original
[manufacturer-linked firmware](https://github.com/IMMRMKW/ramses_esp) against the
received board. It is a wiring/driver reference, not the radiator application.

Fit the antenna before transmission. Use the public RF specification for initial
radio parameters; keep the resulting CC1101 register profile in the firmware's
board/radio configuration with its implementation assumptions.
