# Home Wi-Fi and recovery

## First setup and everyday access

1. Open the USB serial console at 115200 baud. On every boot Cleverbox prints a
   new random 16-character password for its **Cleverbox** recovery access point.
   This is the recovery password, never the home-network password. Keep the boot
   message if you may need recovery without rebooting; there is no board-button
   recovery mechanism or fixed default password.
2. Join **Cleverbox**, then open **http://192.168.4.1/** manually. There is no
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
   the AP connection, the last station URL stays visible and is also on serial.
   The DHCP address may change; check serial or the router's DHCP leases.

Home credentials are held in RAM during trials and saved only after IP acquisition
in a separate, versioned `cleverbox/wifi` NVS key. ESP-IDF Wi-Fi configuration uses
RAM storage so the driver does not independently persist candidate credentials.
There is no application login or HTTPS yet: use a trusted LAN and keep the AP
password private. NVS encryption is not enabled by this firmware. Neither SSID nor
home password appears in status, radio diagnostics or application logs; Wi-Fi
connection logging is suppressed. The recovery password is intentionally serial-only.

## Failure and recovery policy

Each association/DHCP attempt has a **15-second deadline**, including failure to
receive a station-start event. Failed attempts are separated by **5 seconds**, up
to **three attempts**. An immediate driver rejection can finish a batch sooner.
The HTTP page distinguishes `connecting`, `connected` with `ip`, `disconnected`
between attempts, and `failed` after the batch. `setup_result` separately reports
`none`, `pending`, `saved` or `failed`, so a rejected replacement remains visible
even if the old setup later reconnects. `configured` means saved credentials exist,
not that the station is online. Errors contain fixed codes, never credentials.

- Without saved credentials, failed setup leaves the AP open until another explicit
  setup/forget action or a restart. Credentials are not automatically retried forever.
- A failed replacement or failed save preserves the previous saved setup. After a
  **60-second recovery pause**, the old setup is tried again. It remains authoritative
  across restart. A storage failure is shown as `storage_failed`; fix/retry explicitly.
- Loss of station connectivity or its IP reopens the protected AP immediately,
  using the **same password for this boot**, and clears the station IP in status.
  Reconnection starts after 5 seconds. Saved credentials are retried in bounded
  three-attempt batches with 60 seconds between batches. The AP stays available
  throughout a prolonged outage. No reboot loop or flash writes on reconnect.
- A corrupt/unreadable saved Wi-Fi record is reported as `storage_invalid` with
  recovery AP access; no automatic NVS erase. Explicit successful provisioning may
  replace it, or use **Forget home Wi-Fi only**.

**Forget home Wi-Fi only** cancels network setup/retries, disconnects STA and erases
only the Wi-Fi key. It leaves the recovery AP available. It preserves radiator
identities, names, pairings, saved controller state and admitted commands. A failed
erase reports `storage_failed` and retains the current setup; check status before
assuming reset completed. No network event starts pairing, changes targets, or
replays a command.

There is one outstanding configuration action at a time; configuration while a
trial/reconnect is active returns busy. Forget remains available during attempts.
During a retry pause another configuration can be submitted. Wi-Fi events and
requests use a bounded queue owned by a dedicated task; a short periodic wake
handles deadlines without blocking HTTP handlers, the ESP event loop or RF worker.
The station is reset between trials while AP stays enabled. A fresh `STA_START`
is the boundary before association; old disconnect/IP events before it are ignored.
The HTTP server has ten URI slots: root, seven existing routes, network GET and POST.

## HTTP and software validation

- `GET /api/network`: `state`, `setup_result`, `configured`, `recovery_ap`,
  `attempts`, `ip`, `error`. No SSID or password, including after restart.
- `POST /api/network`, JSON action `configure` with `ssid` and `password`, or
  action `forget` alone. Returns 202 for admission, 400 for invalid input and 409
  for busy/unavailable. Read status for the eventual result. Responses are no-store.

`make test test-http test-capacity` exercises the production policy with fake Wi-Fi
operations/events, the real HTTP handlers/page and versioned store. Tests cover
setup/restart, invalid credentials, replacement rollback, timeouts, paced recovery,
late events, corrupt storage, save/forget failures, credential non-disclosure and
RF command completion during network loss and forget. Test credentials are generated
in memory at runtime and temporary storage is deleted. The host adapter does not
associate to a real router: its test-only `network_event` injection in
`/api/simulation` (requires `--test-clock`) delivers `started`, `disconnected` or
`got_ip` to the same policy. It never guesses success from an SSID/password.

**Board acceptance still required:** actual association/authentication and DHCP;
AP accessibility while STA connects, including AP channel changes; IP event ordering
across retries/replacement; AP closure and reopening; router power-off/return;
wrong-password and absent-router boot recovery; power-cycle retention; serial access
and password policy; confirmed physical RF delivery during Wi-Fi loss/reconnection.
The ESP32-C6 build verifies API/link integration, not these radio/network behaviors.
