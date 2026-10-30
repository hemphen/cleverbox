# Home Assistant

Home Assistant connects to Cleverbox through its built-in MQTT integration. No
custom integration is needed. Discovery adds one device per radiator with
read-only entities. Controls come from an optional YAML package.

Schedules, automations, history and notifications stay in Home Assistant.

## Discovery

Each radiator gets a device with these entities. Entity IDs have the form
`<platform>.cleverbox_<bridge_id>_<radiator_id>_<key>`.

| Key | Platform | Meaning |
| --- | --- | --- |
| `temperature` | sensor (°C) | Measured temperature from the latest report |
| `observed_target` | sensor (°C) | Target from the latest report |
| `observed_mode` | sensor | `Comfort`, `Off`, `Frost`, `Eco` or `Unknown` |
| `observation_fresh` | binary sensor, diagnostic | Report is fresh (`fresh` in the snapshot) |
| `report_age` | sensor (s), diagnostic | Report age at the latest snapshot |
| `last_admission` | sensor, diagnostic | Latest admission reason; details in attribute `result` |
| `last_delivery` | sensor, diagnostic | Latest delivery phase; details in attribute `result` |

Entities become unavailable when the bridge is offline or no snapshot has
arrived for 15 s. The three observation sensors are also unavailable when the
report is not fresh or the value is missing. They never show requested
settings.

Discovery configs are published retained at QoS 1 to
`homeassistant/device/cleverbox_<bridge_id>_<radiator_id>/config`. Device and
entity IDs use the bridge ID and registry ID, never the name, so renaming only
changes the label.

## Setup

1. Pair and name the radiators on the Cleverbox web page.
2. Configure the broker on the Cleverbox page and add Home Assistant's MQTT
   integration for the same broker. Keep the default discovery prefix
   `homeassistant` and birth message `homeassistant/status` = `online`.
3. On the Cleverbox page, press **Enable HA discovery**. The devices appear.
4. For controls, set the Cleverbox clock and save a template for each radiator
   (see [templates](architecture.md#templates)).
5. Copy [`examples/home-assistant/cleverbox.yaml`](../examples/home-assistant/cleverbox.yaml)
   into Home Assistant's `packages` directory. Replace every `020000000001` with
   `bridge_id` from `GET /api/mqtt`, and the radiator IDs `1` and `2` with the IDs
   from `GET /api/radiators`. Copy the per-radiator blocks for more radiators.
6. Enable packages in `configuration.yaml`, check the configuration and restart:

   ```yaml
   homeassistant:
     packages: !include_dir_named packages
   ```

7. Optionally add [`examples/home-assistant/dashboard.yaml`](../examples/home-assistant/dashboard.yaml)
   as a manual card, with the same ID replacements.

## Controls

The package adds, per radiator:

- `input_number.cleverbox_<id>_draft_target` (5–30 °C, step 0.5)
- `input_select.cleverbox_<id>_draft_mode` (Comfort, Off, Frost, Eco)
- `button.cleverbox_<id>_apply`

and shared entities `binary_sensor.cleverbox_ready`, `sensor.cleverbox_session`
and `sensor.cleverbox_<id>_snapshot`, plus the script `script.cleverbox_apply`.

Editing a draft sends nothing. **Apply** publishes both draft values once, with
the current session and command token, and never retries. Off sends
`centi_c: 0`. Drafts reset to 21 °C and Comfort when Home Assistant starts and
are never filled from reports.

Apply is available only while the bridge is ready and the radiator snapshot
belongs to the current session. A second Apply before the next snapshot reuses
the consumed token and is rejected with `invalid_command_token`. An Apply while
the radiator is still busy is rejected with
`busy_or_unavailable_or_missing_template`. Neither affects the command already
running. Do not automate retries.

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

Why a package and not an MQTT `climate` entity: MQTT climate, number and select
entities publish target and mode separately, and their command templates cannot
read the current token. Cleverbox requires both values and a valid token in one
message.

## Reading results

- `last_admission` `accepted` means the command was queued; other values are
  the [rejection reasons](mqtt.md#results).
- `last_delivery` `matched` means a later report showed the requested values.
  `timeout` means no required report arrived within 150 s.
- Results are kept in RAM only. Admission results clear when the MQTT session
  changes; the latest delivery result survives reconnects but not reboots.

## Discovery lifecycle

- **Enable or disable**: `POST /api/mqtt` with `discovery_enable` or
  `discovery_disable`. The setting is saved in NVS.
- **New radiator**: its topic is recorded in NVS (`ha_discovery`) before the
  config is published. Changes are checked every 5 s.
- **Rename**: the config is republished under the same topic.
- **Remove or disable**: Cleverbox publishes a retained removal config (each
  component with only its platform) to the recorded topic, waits for the broker's
  PUBACK, then drops the topic from NVS. Removal markers stay on the broker so
  Home Assistant also cleans up after its own restart.
- **Offline**: pending removals survive reboots and run when the same broker is
  reachable again. Changing or forgetting the broker is refused until
  `discovery_pending` is 0.
- **Reconnect or HA restart**: configs and snapshots are republished. Unchanged
  configs are not republished otherwise.
- One config is in flight at a time. A missing PUBACK after 5 s or a failed send
  closes the MQTT client, so an old config cannot overtake a newer removal.
- The NVS journal holds `2 × capacity` radiator IDs. When full, new radiators
  wait until removals complete.

Discovery error codes in `GET /api/mqtt`:

| Code | Meaning | Recovery |
| --- | --- | --- |
| `0x7411` | No PUBACK within 5 s | Client recreated, retried |
| `0x7413` | Publish refused by the client | Client recreated, retried |
| `0x7412` | Config larger than 8191 bytes | Stops discovery until the next enable, disable or reboot |
| other | NVS error | Stops discovery until the next enable, disable or reboot |

## Upgrading from the pre-discovery package

The package before discovery defined observation sensors itself. Replace it
with the current package before enabling discovery, keeping your bridge and
radiator IDs and any customized helper IDs. Delete orphaned old observation
entities in Home Assistant, then enable discovery. Observation entity IDs gain
the bridge ID: `sensor.cleverbox_1_temperature` becomes
`sensor.cleverbox_<bridge_id>_1_temperature`. History is not carried over.

## Testing

`make test-home-assistant` runs Home Assistant Core 2026.10.4 with Mosquitto
and the simulator. It validates the package with Home Assistant's config check,
then exercises discovery, rename, removal, restarts, Apply, rejections, timeouts
and expiry through real service calls:

```sh
python3.14 -m venv build/ha-venv
build/ha-venv/bin/pip install -r host/requirements-ha.txt
PATH="$PWD/build/ha-venv/bin:$PATH" make test-home-assistant
```

Set `CLEVERBOX_HA_TEST_DIR` to keep logs.
