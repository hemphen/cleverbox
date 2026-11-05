# Production hardening (deferred)

Cleverbox is experimental firmware for controlled development: the developer is
present and can read logs, retry, restart, reflash and reset radiators. It is
not suitable for unattended deployment. Production hardening is a later
milestone, after real hardware experience. Do not implement it piecemeal now.

## Risks accepted for now

- **Hardware behaviour is unverified.** RF timing, transmit acceptance,
  generated frames on air and heating behaviour are tested in software only.
- **No automatic radio recovery.** A radio error stops RF until a manual retry
  or restart. Failed commands are not resumed.
- **Silent radio faults go unnoticed.** RadioLib's ESP-IDF HAL does not report
  SPI errors; a dead bus may only show as a setup failure or as silence.
- **Settings writes are not transactional.** A failed NVS write returns an
  error; RAM and flash can then disagree until restart. A corrupt registry
  starts empty and is overwritten by the next change.
- **No interface separation or login.** Anyone on the setup AP (published
  password) or the LAN has full HTTP control. MQTT uses plain TCP. NVS and
  flash are not encrypted. There is no CSRF protection beyond requiring a JSON
  content type.
- **Stale Home Assistant devices.** Discovery removal is published only while
  connected. A radiator removed, or discovery disabled, while offline or before
  a restart leaves its retained config on the broker.
- **One command at a time.** A silent radiator holds the command queue for up
  to 150 s.
- **Reports can be lost** while flash is written or a frame is transmitted.
- **Wrong Wi-Fi credentials** saved from the station network leave the device
  unreachable until BOOT opens the setup AP.
- **No OTA.** Updates need a cable.

## Work for the milestone

- Verify RF behaviour, timing and heating on hardware; measure stack and heap.
- Decide on radio fault detection and bounded automatic recovery, including
  SPI error reporting and a watchdog strategy.
- Make settings persistence robust: verified writes, recovery from corrupt
  records, migration between record versions.
- Add authentication, AP/LAN separation for HTTP, MQTT over TLS and NVS/flash
  encryption with secure boot.
- Make Home Assistant discovery cleanup survive restarts and broker outages.
- Decide whether commands to different radiators need to run concurrently.
- Add OTA updates.
