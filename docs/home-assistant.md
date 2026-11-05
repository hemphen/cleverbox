# Home Assistant

Home Assistant connects to Cleverbox through its built-in MQTT integration.
Discovery adds one device per radiator with read-only entities. Controls come
from an optional YAML package. Schedules and automations stay in Home Assistant.

## Discovery

Enable it with **Enable HA discovery** on the web page
(`{"action":"discovery_enable"}`). Each radiator gets a device; entity IDs are
`<platform>.cleverbox_<bridge_id>_<id>_<key>`.

| Key | Platform | Meaning | Radiators |
| --- | --- | --- | --- |
| `temperature` | sensor (°C) | Measured temperature from the latest report | All |
| `observed_target` | sensor (°C) | Target from the latest report | All |
| `observed_mode` | sensor | `Comfort`, `Off`, `Frost`, `Eco` or `Unknown` | All |
| `observation_fresh` | binary sensor, diagnostic | `fresh` in the snapshot | All |
| `report_age` | sensor (s), diagnostic | Report age at the latest snapshot | All |
| `last_command` | sensor, diagnostic | Latest command phase | Paired |
| `last_admission` | sensor, diagnostic | Latest MQTT admission reason | Paired |

Entities become unavailable when the bridge is offline or no snapshot has
arrived for 15 s. The three reading sensors are also unavailable when the
report is not fresh (old, or radio not ready) or the value is missing. They
never show requested settings.

Configs are published retained, QoS 1, to
`homeassistant/device/cleverbox_<bridge_id>_<id>/config` when MQTT connects,
when Home Assistant sends `online` on `homeassistant/status`, and after a
radiator is added, renamed or removed. Device and entity IDs use the bridge and
registry IDs, so renaming only changes the label. Removing a radiator, or
disabling discovery, publishes an empty retained config, which deletes the
device. This happens only while connected; see
[production hardening](production-hardening.md).

## Setup

1. Pair radiators (disconnect the Touch E3 first), or monitor heard radiators,
   on the Cleverbox page.
2. Configure the broker on the Cleverbox page and add Home Assistant's MQTT
   integration for the same broker. Keep the discovery prefix `homeassistant`
   and the birth message `homeassistant/status` = `online`.
3. Press **Enable HA discovery**. The devices appear.
4. For controls, copy [`examples/home-assistant/cleverbox.yaml`](../examples/home-assistant/cleverbox.yaml)
   into Home Assistant's `packages` directory. Replace every `020000000001` with
   `bridge_id` from `GET /api/mqtt`, and the radiator IDs `1` and `2` with
   yours. Copy the per-radiator blocks for more radiators.
5. Enable packages in `configuration.yaml`, check the configuration and restart:

   ```yaml
   homeassistant:
     packages: !include_dir_named packages
   ```

6. Optionally add [`examples/home-assistant/dashboard.yaml`](../examples/home-assistant/dashboard.yaml)
   as a manual card, with the same replacements.

## Controls

The package adds, per radiator, `input_number.cleverbox_<id>_draft_target`
(5–30 °C), `input_select.cleverbox_<id>_draft_mode` and
`button.cleverbox_<id>_apply`, plus `binary_sensor.cleverbox_ready` (radio
ready and device time usable) and `script.cleverbox_apply`.

Editing a draft sends nothing. **Apply** publishes both draft values once and
never retries. Off sends `centi_c: 0`. Apply is unavailable while Cleverbox is
not ready. A second Apply while the radiator's command is still running is
rejected as `busy`.

Automations set the drafts and press Apply:

```yaml
sequence:
  - action: input_number.set_value
    target:
      entity_id: input_number.cleverbox_1_draft_target
    data:
      value: 20.5
  - action: input_select.select_option
    target:
      entity_id: input_select.cleverbox_1_draft_mode
    data:
      option: Comfort
  - action: button.press
    target:
      entity_id: button.cleverbox_1_apply
```

The package is used instead of an MQTT `climate` entity because MQTT climate
entities publish target and mode separately, and Cleverbox needs both in one
command.

## Reading results

- `last_admission` `accepted` means the command was queued; other values are
  [rejection reasons](mqtt.md#commands).
- `last_command` `matched` means a later report showed the requested values.
  `timeout` means no required report arrived within 150 s.

## Testing

`make test-home-assistant` runs Home Assistant Core 2026.10.4 with Mosquitto
and the simulator. It checks the package with Home Assistant's config check,
then exercises discovery of paired and monitored radiators, rename, Apply,
radio failure, snapshot expiry, removal and discovery disable:

```sh
python3.14 -m venv build/ha-venv
build/ha-venv/bin/pip install -r host/requirements-ha.txt
PATH="$PWD/build/ha-venv/bin:$PATH" make test-home-assistant
```

Set `CLEVERBOX_HA_TEST_DIR` to keep logs.
