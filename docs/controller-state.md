# Controller-state construction boundary

The working contract is clevertouch-protocol
[`b70fab79300544a62ee2606bd4055cdb81763fd4`, RF specification](https://github.com/hemphen/clevertouch-protocol/blob/b70fab79300544a62ee2606bd4055cdb81763fd4/docs/radiator-rf/specification.md),
especially **Frame envelope**, **Record reference**, **Opaque state and clock
fields**, **Sending and receiving boundaries**, and **Synthetic frame examples**.
The implementation reference is clevertouch-radio
[`7706b90c2c1f7dcd48b16d30efabb20e7d93671b`, decoder.py](https://github.com/hemphen/clevertouch-radio/blob/7706b90c2c1f7dcd48b16d30efabb20e7d93671b/radiatord/decoder.py):
`crc16` and `decode_packet` establish parsing/checksum behavior.
Its [README at that revision](https://github.com/hemphen/clevertouch-radio/blob/7706b90c2c1f7dcd48b16d30efabb20e7d93671b/README.md)
explicitly says RF transmission is not implemented. It supplies no transmitter
defaults for unresolved fields. These sources share evidence; they are not
independent physical validation.

## Field decisions

Offsets include the length byte. Tags and their order are fixed by the documented
59-byte layout; value offsets below account for the complete payload.

| Bytes | Field | Evidence and current construction policy |
| --- | --- | --- |
| 0, 5 | Length `38`, marker `C6` | Defined envelope/layout; validated before use. |
| 1–4, 6–9 | Source, destination | Saved pairing; overwritten for each live transmission. |
| 11–12, 17, 19–20 | `45`, `3B`, `4C` values | Observed zeros, opaque meanings; preserve learned/imported values. |
| 14–15 | `4B` | Raw new radiator report temperature, including `084C` unavailable. |
| 22–24, 26–29 | `8E`, `CC` | Observed `000084`, `00000000`; sample constants, not universal defaults. Preserve. |
| 31–33 | `97` | Unresolved controller word. Byte 0 correlated with report `DF` byte 0; this does not define the other bytes or a derivation. Preserve. |
| 35–37 | `94` | Defined target/mode; nearest integer raw `320 + 18*C`, Off raw 320. Existing 5–30 °C limits and Comfort/Off/Frost/Eco support remain. |
| 39–42 | `E1` | Copy raw bytes from the new report. Echo policy remains an explicit prototype assumption; no thermostat/duty computation. |
| 44–47 | `DA` | Zero as in sampled controller states; existing explicit policy, not a proven reset operation. |
| 49–50 | `5B` | Unresolved per-device word, varying slightly over time; neither a default nor a report-derived rule exists. Preserve. |
| 52–54 | `B2` | Binary local H/M/S; client seconds since midnight plus elapsed monotonic time, wrapping at midnight. No inferred timezone/date. |
| 55–56, 57–58 | Inner, outer CRC | Recalculate MODBUS over 1–54, store LE; then CMS over 0–56, store BE. |

No new default bytes are adopted. The synthetic example's `97=203004` and
`5B=0078` are fixture values, not construction rules. Even sample-constant fields
are preserved, allowing a different valid template without silently normalizing it.

## Supported partial improvement and exact blocker

A new radio reception can supply the missing controller fields without manual
pasting. Only a CRC-valid full controller-state layout with both saved addresses
is learned. Reports, acknowledgements, corrupt frames and other addresses cannot
replace it. Learning never changes requested or observed state, and never sends.
Commands snapshot their selected frame at admission: explicit per-command import,
then latest learned RAM state, then explicitly saved state. A new learned frame
wins in RAM but cannot silently replace saved flash state. Saving/importing never
starts a command. The existing fresh-report requirement,
address checks, clock refresh, two-attempt bound and later matching report
semantics remain unchanged. Pending commands and observations remain transient.

Fully template-free control of an arbitrary newly paired radiator is still
blocked by **five value bytes: `97` at 31–33 and `5B` at 49–50**. The protocol
does not justify zeroing them, using the synthetic example, or constructing them
from `DF`. Learning/import remains necessary for that concrete reason. Reusing
opaque values while they may drift is itself an unverified prototype assumption;
the UI exposes template age without inventing a protocol expiry interval.

## Explicit one-radiator storage

Use **Save learned state / retry** after learning, or **Import and save / retry**
with a complete same-radiator frame. Wait for **Latest explicit save: succeeded**
before restarting. The save button snapshots learned RAM state when admitted;
an imported save validates both CRCs and all tags/lengths in the supported 59-byte
layout, plus the radiator destination. It preserves the entire original frame.
As with existing per-command imports, an old controller source is allowed; only
live transmission substitutes the paired controller source and recalculates CRCs.
Passive learning still requires both paired addresses to match.

The `cleverbox/pairing` version-1 nine-byte record is unchanged. The optional
`cleverbox/control_state` record is version byte `01`, paired controller (4), paired
radiator (4), and exact frame (59): 68 bytes, independent of C padding. Both saved
identities must match the loaded pair; CRC/layout/destination validation runs
again on boot. An identity-only nine-byte record is an explicit clear tombstone.
There is no registry, automatic migration write or automatic reset. Missing state
is normal for existing pairing-only installations. Unknown/corrupt state is shown
as a load error without changing pairing or overwriting the bad record; learning
and per-command imports remain available. Unknown/corrupt pairing blocks inclusion.
An explicit save/import/clear may replace a readable state record; unreadable or
oversized records block writes rather than risking their contents.

Storage work uses the existing radio worker queue. RAM's saved fallback changes
only after a successful write and commit. Write failures leave the old fallback;
commit failures trigger restoration of the previous bytes (or an empty tombstone
if previously absent), because NVS may write before commit. Failure is reported,
and the same save/import can be retried without pairing or RF. If storage also
fails during restoration, an invalid-state error is returned: the old RAM fallback
remains usable, but on-disk recovery cannot be guaranteed under continuing storage
failure. Physical flash/power-loss durability still requires board testing.

**Clear saved and learned state** commits a tombstone, then drops learned RAM state
and saved fallback while keeping pairing. A failed clear keeps both available.
An already admitted command retains its snapshot; later reception can learn again.
The browser import text is independent and remains a per-command override until
cleared. Successful import-and-save updates saved fallback, not learned RAM, so
newer learned RAM still takes precedence according to the policy above.

The status API distinguishes `missing`, `learned` and `saved` configuration, save/
clear outcomes and storage errors. Save outcomes describe this boot's explicit
attempts and reset to `not_attempted` on restart; they are not a saved audit log.
Only learned state has monotonic capture age. Saved capture age is unknown, and
loading it creates no radiator observation. Boot loads configuration and starts RX
only: no inclusion, polling, settings transmission or pending-command replay.
Saving captured `97`/`5B` avoids repeated capture; it neither proves indefinite
validity nor enables capture-free installation. No synthetic defaults are added.

## Software and physical validation

`test_control.c` compares whole live frames with the specification's synthetic
example and separately assembled vectors: fixed record concatenation, raw target
words 410/689/860/320/446/599 and 23:59:59 clock bytes `17 3B 3B`. Their CRCs were
calculated with the pinned reference `decoder.py` `crc16`, independently of the
production builder. Further complete vectors cover unavailable temperature,
changed raw E1 and changed controller address. No household frames are fixtures.
Transport tests feed frames through the real receive worker and check learning,
rejection, snapshot ownership, command admission and existing bounded delivery.

Software consistency does not establish custom-transmitter acceptance, RF timing,
pairing persistence on physical flash, heating behavior, thermostat ownership or
continued operation without Touch E3. All need hardware and real radiators.
