#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Test-only JSON-lines control of a real HA Core process; no mocked HA APIs.

Bootstraps the on-disk package and uses HA's MQTT config flow and service bus.
The parent owns broker/RF simulation; no HA component or integration is replaced.
"""
import asyncio
import json
import sys
from homeassistant.bootstrap import async_setup_hass
from homeassistant.runner import RuntimeConfig
from homeassistant.const import __version__


async def main():
    assert __version__ == '2026.10.4', __version__
    hass = await async_setup_hass(RuntimeConfig(config_dir=sys.argv[1], log_file=sys.argv[1]+'/home-assistant.log'))
    assert hass is not None and not hass.config.recovery_mode
    await hass.async_start()
    try:
        if not hass.config_entries.async_entries('mqtt'):
            result = await hass.config_entries.flow.async_init('mqtt', context={'source': 'user'})
            result = await hass.config_entries.flow.async_configure(result['flow_id'], {
                'broker': '127.0.0.1', 'port': int(sys.argv[2]), 'username': '', 'password': '', 'other_settings': {'set_client_cert': False, 'set_ca_cert': 'off'}})
            assert result['type'] == 'create_entry', result
        await hass.async_block_till_done()
        print(json.dumps({'ready': __version__}), flush=True)
        while line := await asyncio.to_thread(sys.stdin.readline):
            req = json.loads(line)
            try:
                if req['op'] == 'stop':
                    break
                if req['op'] == 'call':
                    await hass.services.async_call(req['domain'], req['service'], req.get('data', {}), blocking=True)
                    await hass.async_block_till_done()
                    result = {'ok': True}
                elif req['op'] == 'registries':
                    from homeassistant.helpers import entity_registry as er, device_registry as dr
                    result = {
                        'entities': [dict(entity_id=e.entity_id, unique_id=e.unique_id, device_id=e.device_id)
                                     for e in er.async_get(hass).entities.values()],
                        'devices': [dict(id=d.id, name=d.name, identifiers=list(d.identifiers))
                                    for d in dr.async_get(hass).devices]}
                elif req['op'] == 'states':
                    result = {s.entity_id: {'state': s.state, 'attributes': dict(s.attributes)} for s in hass.states.async_all()}
                else:
                    raise ValueError(req)
            except Exception as exc:
                result = {'error': str(exc)}
            print(json.dumps(result, default=str), flush=True)
    finally:
        await hass.async_stop()


if __name__ == '__main__':
    asyncio.run(main())
