# Device time

Normal immediate commands use Cleverbox time. An unsynchronized boot has **no
usable time**, even if an RTC or host clock happens to contain a plausible date.
Connect home Wi-Fi for SNTP, or use **Device time → Offline bench** to explicitly
set UTC. Commands without usable time return 409 with those recovery actions;
they do not queue a poll, send midnight, or start pairing.

## UTC, local time and synchronization

ESP-IDF v5.5.1's `esp_netif_sntp` service is initialized before Wi-Fi, with
`start=false`, `wait_for_sync=false` and immediate (not smooth) updates. Station
IP acquisition starts/restarts SNTP against `pool.ntp.org`. There is no wait for
DNS, a server reply or synchronization in startup, HTTP handlers or the RF worker.
The IDF service performs periodic updates (its default one-hour interval) and
handles unavailable servers asynchronously. Reconnection requests another sync;
it never creates radiator commands. There is no cloud/location service or new
network dependency to install; SNTP comes with the pinned ESP-IDF.

SNTP sets the system UTC epoch. The successful-sync callback anchors the
application clock to that UTC epoch and `esp_timer_get_time()`. Explicit manual
UTC uses `settimeofday()` and the same anchor. The shared clock logic advances
UTC by elapsed monotonic time, then uses `localtime_r()` for the configured
POSIX timezone. Anchoring only from those two successful events makes boot
validity/provenance explicit and prevents unrelated RTC values from becoming
usable silently. It also makes the clock boundary injectable without setting
the development computer's system clock. Resolution is one second; fractional
SNTP seconds are discarded. A date from 2020 through 2099 is accepted; invalid
sync dates do not establish or replace usable time. A clock advancing outside
that range becomes unusable. There is no accuracy guarantee during an outage.

`GET /api/time` and the page show:

- `usable`, `source`: `unsynchronized`, `manual` or `network`.
- Current `utc`, `local` (including UTC offset), and `unix_seconds`, only when usable.
- `timezone`, always; default **`UTC0`**, without DST.
- `synchronized_this_boot`: whether any valid SNTP callback occurred this boot.
- `sync_age_ms`: monotonic age of the latest valid network sync, or null before one.
- `network_connected`, independently of clock validity. This means station IP
  connectivity, not proof that an NTP server is reachable.
- `sntp_error` for SNTP setup/start errors, `config_error` for timezone load/save
  errors, and `persistence_uncertain` after a failed timezone write/readback.

An offline, previously synchronized clock stays usable and marked `network`;
its sync age grows. A manual set is marked `manual`, even if an earlier network
sync happened this boot: the historical sync flag/age remain true/visible.
A subsequent successful network sync replaces manual time. A server outage can
also occur while Wi-Fi remains connected: the growing sync age is the evidence;
connectivity does not mean a fresh sync. No arbitrary age threshold claims
accuracy, expires a command, or turns an unsynchronized boot into synchronized.

## Timezone configuration and DST

`POST /api/time` with `{"action":"timezone","timezone":"UTC0"}` explicitly saves
configuration. Sweden, including current EU DST transitions, can use:

```json
{"action":"timezone","timezone":"CET-1CEST,M3.5.0/2,M10.5.0/3"}
```

This is a **POSIX rule**, not an IANA name such as `Europe/Stockholm`. The POSIX
offset sign is reversed: `CET-1` is UTC+1. A rule, unlike a database, will not
automatically learn future legal timezone/DST changes; update it explicitly if
needed. DST follows the configured rule at conversion time. The application
keeps UTC internally; no DST hour is added to the system epoch.

The validated subset (maximum 95 ASCII bytes) is:
`STDoffset[DST[offset],Mmonth.week.day[/time],Mmonth.week.day[/time]]`.
Names contain 3–10 letters. Offsets are optional-sign `h[:m[:s]]` with hours
0–23 and minutes/seconds 0–59. Standard offset is mandatory; omitted DST offset
means one hour ahead of standard time. Both DST transitions are mandatory when
DST is named. `M` month is 1–12, week 1–5 (5 means last), weekday 0–6 (Sunday=0).
Transition time is unsigned `h[:m[:s]]` in 0–23:59:59, default 02:00:00, expressed
in local time before the transition. `J`/day-of-year rules, bracketed names,
negative/extended transition hours, whitespace and implicit libc DST rules are
not accepted. These limits keep behavior shared by ESP-IDF newlib and host libc.

Only the versioned `cleverbox/timezone` NVS key is written, on explicit saves.
No UTC timestamp, manual clock, sync flag or age is stored. Each explicit save
erases and commits the timezone key before rewriting, committing and verifying
through a fresh handle. This forces a new write even when retrying the cached
old rule after a failed write whose effect may be deferred until restart.
An ordinary same-value write/readback is not treated as resolving uncertainty.
On error the active RAM rule is retained, with explicit
persistence uncertainty: storage may contain the old/new/missing/invalid rule
across restart, and there is no rollback or power-loss guarantee. Retry save and
check status. At boot, the actual saved rule is validated; malformed/unreadable
configuration is reported and falls back to UTC0. No automatic NVS erase occurs.
Wi-Fi forget and radiator storage operations leave this key alone.

## Explicit manual time and compatibility

For offline bench use, enter **UTC** in the page's manual input and click
**Set manual UTC time**, or send:

```json
{"action":"manual","unix_seconds":1767268800}
```

This sets 2026-01-01 12:00:00 UTC for this boot only. It is not a network sync.
Both time actions return 200 with `transmitted:false` on success, 400 for invalid
input and 409 on an update/storage error. Check status after failures. Local HTTP
uses the existing trusted-LAN security model, without a new login. Status/actions
are no-store. The input is never automatically filled from the browser clock.

Normal `POST /api/command` now omits `clock_seconds`:

```json
{"id":1,"mode":0,"centi_c":2050}
```

For existing clients and deterministic RF tests only, an explicitly supplied
integer `clock_seconds` (0–86399) **overrides device time**, even when the device
clock is unavailable. Explicit null, string, fraction and out-of-range values
fail rather than falling back. This override does not set device time or change
its provenance. The normal page never sends it. Firmware interfaces should use
`radio_control_start_device()`; `radio_control_start()` retains explicit-clock
admission for compatibility/testing. Controller-template selection is unchanged:
per-command import → learned RAM state → saved fallback. Time setup does not
modify templates, requested settings, observations or pairing.

## Admitted commands and monotonic timers

Admission snapshots **local seconds since midnight and the monotonic sample
instant**, along with the existing selected template and requested settings.
Each actual state TX, including a retry, adds elapsed monotonic whole seconds
since that sample and wraps modulo 86400. Queue residence is included. After
admission, network/manual forward or backward corrections, timezone edits and
DST changes do not alter that command's clock. New commands use the corrected
clock and current timezone. A pending command spanning a DST transition therefore
continues its original local clock linearly. This simple bounded-delivery policy
avoids changing a command's meaning partway through retries.

The protocol reference defines `B2` as binary hour/minute/second and does not
establish a date or timezone field. Sending configured local time is this
appliance's explicit policy; no extra date or scheduling records are invented.
See the [working field reference](https://github.com/hemphen/clevertouch-protocol/blob/main/docs/radiator-rf/specification.md#opaque-state-and-clock-fields).

Report deadlines, retry intervals, observation/template ages, pairing delays and
Wi-Fi recovery timers continue to use `esp_timer_get_time()`. Wall corrections
cannot expire, restart or replay delivery. A successful sync, timezone save,
manual set, reconnect or reboot never itself sends RF. Commands and observations
are not restored after reboot; saved radiator configuration is restored as before.

## Software validation and remaining board checks

`make test test-http test-capacity` runs the production time and command logic
with injected monotonic time, UTC-setting and successful-sync boundaries, plus
existing codec, pairing, registry, network, storage and UI tests. Coverage includes
unset boot; manual/source transitions; first sync; offline continuation;
reconnect/resync; forward/backward corrections; exact command and network timer
boundaries; queue residence; report-driven retries and midnight wrap; spring/fall
DST conversion; invalid HTTP/POSIX input; timezone persistence, corrupt storage,
write errors, deferred-write retries surviving NVS reinitialization, failed
erase/commit/verification retaining uncertainty, and restart without automatic RF.
The page script is executed with
a DOM/fetch boundary. The host simulator never actually contacts an NTP server
or sets host system time: interactive use starts unset and needs a manual action.
With `--test-clock` only, `time_sync` plus integer `unix_seconds` in
`/api/simulation` injects a successful synchronization callback.

The ESP32-C6 firmware build checks IDF API and linker integration. **Hardware
acceptance remains pending:** actual DNS/SNTP replies and repeated sync,
blocked/unreachable NTP with HTTP/RF still working, router outage/reconnection,
manual UTC and newlib timezone/DST display on the board, flash retention across
power cycles, and physical radiator B2/target/mode delivery with Touch E3 removed.
Clock drift, radio acceptance and continued heating behavior require measurement.
[MQTT transport](mqtt.md) now uses the same device-time admission. HA discovery
follows separately; no schedule engine, Matter, OTA or history storage is included.
