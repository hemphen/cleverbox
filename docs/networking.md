# Home Wi-Fi and recovery

## First setup and everyday access

1. Join the **Cleverbox** setup/recovery AP with WPA2 password **cleverbox**.
   SSID and password are fixed and intentionally public on first boot, ordinary
   restart, network loss, runtime recovery and Wi-Fi forget. No serial monitor,
   per-device secret or per-boot password is required.
2. Open **http://192.168.4.1/** manually. There is no
   scanning, DNS redirect or captive-portal detection. The AP uses WPA2-PSK and
   allows two clients. It is available immediately at boot, including while saved
   credentials are tried.
3. In **Home Wi-Fi**, enter the SSID exactly (no trimming) and its password. This
   milestone supports 2.4 GHz WPA2 personal networks, not open/enterprise Wi-Fi or
   raw 64-digit PSKs. SSIDs use 1–32 bytes; passwords use 8–63 printable ASCII
   characters. UTF-8 SSIDs count bytes. Escaped NUL is rejected.
4. Click **Connect and save**. HTTP admission is asynchronous; check **Setup
   result**, **Wi-Fi** state and **Error**. An accepted request is not a successful
   connection. The page never fills the password back in and clears it on submit.
5. Once DHCP supplies an IPv4 address, Cleverbox saves the candidate and shows/logs
   the station URL. The same HTTP application listens on both interfaces. Join
   the home network and open that URL. The AP closes **60 seconds after IP
   acquisition**, including on a normal boot/reconnection. If the browser loses
   the AP connection, the last station URL stays visible.
   The DHCP address may change; check the router's DHCP leases.

Home credentials are held in RAM during trials and saved only after IP acquisition
in a separate, versioned `cleverbox/wifi` NVS key. ESP-IDF Wi-Fi configuration uses
RAM storage so the driver does not independently persist candidate credentials.
There is no application login or HTTPS yet: use a trusted LAN. The generic AP
password is public documentation, not a per-device access secret. NVS encryption
is not enabled by this firmware. Neither SSID nor
home password appears in status, radio diagnostics or application logs; Wi-Fi
connection logging is suppressed.

## Runtime button recovery and restart

After normal startup with BOOT released, hold **BOOT/IO9 continuously for at least
five seconds, then release**. Recovery opens the AP without restarting or stopping
STA. `manual_recovery:true` keeps it open beyond the ordinary AP-close timer and
through reconnect/DHCP events. Settings, radiator identities/names/pairings,
controller templates, MQTT, timezone and discovery cleanup remain untouched.
No RF command or pairing is created, cancelled or replayed; admitted work continues.

**Finish recovery** releases this volatile latch and resumes automatic AP policy.
When configured STA has a usable IP and Wi-Fi persistence is trusted, a fresh
60-second grace period gives time to switch networks before AP closure. Otherwise
AP access remains open (unconfigured, disconnected, connecting or uncertain).
A successful explicit Wi-Fi configuration save after DHCP may release the latch;
failed saves, reconnect and DHCP renewal cannot. Failed AP opening reports
`recovery_error:"ap_open_failed"` and retries at a bounded one-second pace while
latched. Status separates the requested latch from actual `recovery_ap` state.

**EN** and power cycling restart, retain saved configuration and restore the
startup AP window with the same **Cleverbox / cleverbox** credentials. Manual
recovery does not survive reboot; pending commands and observations are never
restored. EN is hardware reset, not an application GPIO; no distinction between EN
and power removal is promised. **Do not hold BOOT at reset:** GPIO9 is a strapping
pin and may select ROM download mode. Runtime recovery requires a stable released
state after startup before it arms, then a new deliberate press/release. Stuck-low
input cannot trigger repeated actions. Short presses and contact bounce are ignored.

Supplied-enclosure BOOT access is **not yet verified**. Opening the enclosure or
a verified access hole may be necessary; do not assume a hole location. EN plus
the startup AP window remains an alternative without serial. The
[hardware checklist and manufacturer button evidence](hardware.md#boot--en-reference-and-physical-acceptance-pending)
cover physical verification. There is no destructive button gesture/factory reset.

| Action | Preserves | Erases / interrupts |
| --- | --- | --- |
| Runtime BOOT hold/release | All saved setup, STA and admitted RF work | Nothing |
| Finish recovery | All configuration and admitted RF work | Volatile manual latch only; automatic AP policy resumes |
| EN / power cycle | Radiator identities/pairings/templates, Wi-Fi, MQTT, timezone, discovery cleanup | Volatile observations, manual latch and pending work; no replay |
| Confirmed Forget home Wi-Fi only | Radiators/templates, MQTT settings/credentials, timezone, discovery cleanup and admitted RF work | Only Wi-Fi key and STA setup/retries, on verified success |

## Failure and recovery policy

Each association/DHCP attempt has a **15-second deadline**, including failure to
receive a station-start event. Failed attempts are separated by **5 seconds**, up
to **three attempts**. An immediate driver rejection can finish a batch sooner.
The HTTP page distinguishes `connecting`, `connected` with `ip`, `disconnected`
between attempts, and `failed` after the batch. `setup_result` separately reports
`none`, `pending`, `saved` or `failed`, so a rejected replacement remains visible
even if the old setup later reconnects. Normally `configured` means saved credentials
exist, not that the station is online. When `persistence_uncertain` is true it means
only that this boot has a configured RAM fallback; the saved record after restart
may be old, new, absent or invalid. Errors contain fixed codes, never credentials.

- Without saved credentials, failed setup leaves the AP open until another explicit
  setup/forget action or a restart. Credentials are not automatically retried forever.
- A failed connection trial does not write flash and preserves the previous setup.
  After a **60-second recovery pause**, the old setup is tried again.
- A failed Wi-Fi write/commit is **not** assumed to leave storage unchanged. The
  store snapshots the previous raw Wi-Fi record (or its absence), attempts to restore
  it, commits and checks it using a fresh NVS handle. Restoring an existing record
  erases then rewrites it so NVS's same-value cache optimization cannot skip the
  restoring write. A verified rollback reports `storage_failed`; the old setup is
  retained. No partition erase/reinitialization is attempted: other components use NVS.
- If restoration or verification fails, status reports `persistence_uncertain: true`
  and error `persistence_uncertain`. The policy retains only the old **RAM** fallback;
  there is **no promise about which record will load after restart**. The protected
  AP stays open even when that fallback reconnects, and the warning survives retries
  and DHCP renewal. A new verified save or verified forget clears it. If forget finds
  only cached absence after an uncertain error, it cannot certify deletion; provision
  successfully or restart, inspect status and retry forget. A previously oversized
  corrupt key cannot be backed up within this bounded store; failed repair also reports
  uncertainty. Successful repair/reset is still allowed.
- Loss of station connectivity or its IP reopens the protected AP immediately,
  using the **same fixed password cleverbox**, and clears the station IP in status.
  Reconnection starts after 5 seconds. Saved credentials are retried in bounded
  three-attempt batches with 60 seconds between batches. The AP stays available
  throughout a prolonged outage. No reboot loop or flash writes on reconnect.
- A corrupt/unreadable saved Wi-Fi record is reported as `storage_invalid` with
  recovery AP access; no automatic NVS erase. Explicit successful provisioning may
  replace it, or use **Forget home Wi-Fi only**.

These are best-effort software-error recovery steps, not a power-loss transaction:
interrupting a write/rollback can leave an old, new, missing or invalid record. At
boot Cleverbox reads what NVS actually exposes and supplies its recovery AP.

**Forget home Wi-Fi only** requires explicit browser confirmation and HTTP
`confirm:true`. On success it cancels network setup/retries, disconnects STA and erases
only the Wi-Fi key. It latches recovery AP access even when the erase fails. It
preserves radiator
identities, names, pairings, saved controller state, MQTT credentials/settings,
timezone, discovery cleanup and admitted commands. A failed
erase also attempts restoration and verification of the previous record. A verified
rollback reports `storage_failed`; failed restoration reports `persistence_uncertain`
and keeps recovery access available. Check status before assuming reset completed.
No network event starts pairing, changes targets, or replays a command.

There is one outstanding configuration action at a time; configuration while a
trial/reconnect is active returns busy. Forget remains available during attempts.
During a retry pause another configuration can be submitted. Wi-Fi events and
requests use a 16-slot bounded queue owned by the network task; Finish recovery
and forget remain admissible during connection attempts when no other action is
pending. A full queue or failed admission returns HTTP 409 for explicit retry.
Lost Wi-Fi events increment the saturating `event_queue_overflows` counter and
invoke existing bounded network recovery. The owner samples GPIO9 on each
iteration, with at most a 20 ms queue wait, so button recovery needs no queue slot
or additional task/ISR. The small portable button policy uses 40 ms debounce,
monotonic timestamps and a 5000 ms hold, measured from the first sampled low to
the first sampled high after accepted press debounce; release debounce does not
round a short hold up. Physical timing has polling resolution. No Wi-Fi/NVS/RF
work runs in interrupt context.
The station is reset between trials while AP stays enabled. A fresh `STA_START`
is the boundary before association; old disconnect/IP events before it are ignored.
The HTTP server has fourteen URI slots: root, seven control routes, network GET/POST,
device-time GET/POST and MQTT GET/POST. Device time uses nonblocking SNTP on station connection;
see [time setup and offline operation](time.md). No network/time event creates an RF command.

## HTTP and software validation

- `GET /api/network`: `state`, `setup_result`, `configured`, `recovery_ap`,
  `attempts`, `ip`, `error`, `persistence_uncertain`, `manual_recovery`,
  `recovery_error`, `event_queue_overflows`. No SSID or password, including after restart.
- `POST /api/network`, JSON action `configure` with `ssid` and `password`, or
  action `finish_recovery` alone, or `{"action":"forget","confirm":true}`.
  Returns 202 for admission, 400 for invalid input and 409
  for busy/unavailable. Read status for the eventual result. Responses are no-store.

`make test test-http test-capacity` exercises the production policy with fake Wi-Fi
operations/events, the real HTTP handlers/page and versioned store. Tests cover
setup/restart, invalid credentials, replacement rollback, timeouts, paced recovery,
late events, corrupt storage, save/forget failures (including errors after durable
mutation, deferred effects on NVS reinitialization and failed rollback/readback),
uncertainty status/AP retention, button debounce/hold/release/startup arming,
manual recovery through DHCP/reconnect and provisioning, safe finish, bounded
queue pressure/AP retry failures, unrelated-key restart isolation, credential non-disclosure and
RF command completion during network loss and forget. Test credentials are generated
in memory at runtime and temporary storage is deleted. The host adapter does not
associate to a real router: its test-only `network_event` injection in
`/api/simulation` (requires `--test-clock`) delivers `started`, `disconnected` or
`got_ip` to the same policy. It never guesses success from an SSID/password. With
`--test-clock`, `wifi_storage_fault` modes 1 and 2 inject a Wi-Fi-key mutation followed
by an error, with mode 2 also failing rollback; mode 0 disables the fault. These are
host-only NVS boundary faults, not a second storage policy.
`button_level` with boolean `low` plus clock advancement feeds the production
button policy; it does not test physical GPIO. The adapter unit test additionally
executes the production network-task iteration with fake GPIO/RTOS/Wi-Fi calls.

**Board acceptance still required:** actual association/authentication and DHCP;
AP accessibility while STA connects, including AP channel changes; IP event ordering
across retries/replacement; AP closure and reopening; router power-off/return;
wrong-password and absent-router boot recovery; power-cycle retention; GPIO9 polarity/boot strapping, runtime hold/release, EN
restart, enclosure access and phone setup without serial; confirmed physical RF delivery during Wi-Fi loss/reconnection.
The ESP32-C6 build verifies API/link integration, not these radio/network behaviors.
