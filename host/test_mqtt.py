#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""MQTT commands, snapshots and Home Assistant discovery with a fake client."""
import json
from pathlib import Path
import tempfile
from test_http import App, seed_pairing, report_from

BASE = 'cleverbox/v1/020000000001'
BROKER = {'action': 'configure', 'host': 'broker.test', 'port': 1883, 'username': '', 'password': '', 'enabled': True}


def drain(app):
    rows = app.http('/api/simulation/mqtt')
    for row in rows:
        row['payload'] = json.loads(row['payload']) if row['payload'].startswith('{') else row['payload']
    return rows


def send(app, payload, id=1, retained=False, topic=None):
    app.sim('mqtt_message', topic=topic or f'{BASE}/radiators/{id}/set', payload=json.dumps(payload), retained=retained)
    return [r['payload'] for r in drain(app) if r['topic'].endswith(('/result', '/errors'))][-1]


def run(directory):
    storage = Path(directory) / 'mqtt'
    seed_pairing(storage)
    app = App(storage)
    try:
        app.http('/api/mqtt', BROKER)
        app.advance(200)
        app.sim('mqtt_connection', connected=True)
        assert app.http('/api/mqtt')['connected']
        rows = drain(app)
        assert {'topic': f'{BASE}/availability', 'payload': 'online', 'qos': 1, 'retain': True} in rows
        # Snapshots right after connecting and every five seconds; no discovery yet.
        app.advance(200)
        rows = drain(app)
        assert not any(r['topic'].startswith('homeassistant/') for r in rows)
        bridge = next(r['payload'] for r in rows if r['topic'] == f'{BASE}/bridge')
        assert bridge['radio_ready'] and not bridge['time_usable']
        state = next(r['payload'] for r in rows if r['topic'] == f'{BASE}/radiators/1/state')
        assert state == {'id': 1, 'name': 'Radiator', 'radiator': 'F1040506', 'paired': True, 'radio_ready': True,
                         'available': False, 'fresh': False, 'command': {'phase': 'idle', 'attempts': 0, 'error': 0}}
        app.advance(4800)
        assert not any(r['topic'].endswith('/state') for r in drain(app))
        app.advance(200)
        assert any(r['topic'].endswith('/state') for r in drain(app))
        # Commands: admission result now, delivery in the snapshots.
        command = {'mode': 0, 'centi_c': 2050, 'request_id': 7}
        assert send(app, command) == {'request_id': 7, 'accepted': False, 'reason': 'time_unavailable'}
        app.http('/api/time', {'action': 'manual', 'unix_seconds': 1767225600})
        assert send(app, command, retained=True) == {'request_id': 7, 'accepted': False, 'reason': 'retained_command'}
        assert send(app, {'mode': 0}) == {'accepted': False, 'reason': 'invalid_request'}
        assert send(app, {'mode': 0, 'centi_c': 2050, 'request_id': -1}) == {'accepted': False, 'reason': 'invalid_request'}
        assert send(app, command, id=9) == {'request_id': 7, 'accepted': False, 'reason': 'unknown_radiator'}
        assert send(app, command, topic=f'{BASE}/radiators/x/set')['reason'] == 'invalid_topic'
        assert app.http()['simulation_tx'] == []
        assert send(app, command) == {'request_id': 7, 'accepted': True, 'reason': 'accepted'}
        assert send(app, {'mode': 3, 'centi_c': 1900}) == {'accepted': False, 'reason': 'busy'}
        app.advance(1000)
        assert app.http()['radiators'][0]['command']['phase'] == 'matched'
        app.advance(5000)
        state = [r['payload'] for r in drain(app) if r['topic'] == f'{BASE}/radiators/1/state'][-1]
        assert state['command'] == {'phase': 'matched', 'attempts': 1, 'error': 0, 'requested_mode': 0,
                                    'requested_centi_c': 2050, 'request_id': 7}
        assert state['available'] and state['fresh'] and state['target_centi_c'] == 2050 and state['controller'] == 'F1010203'
        # Discovery: one device per radiator; monitored radiators get no command entities.
        app.sim('inject', frame=report_from('F1445566', 'D0123456'))
        app.advance(5)
        app.http('/api/radiators', {'action': 'monitor', 'radiator': 'F1445566', 'name': 'Hall'})
        app.http('/api/mqtt', {'action': 'discovery_enable'})
        app.advance(200)
        configs = {r['topic']: r for r in drain(app) if r['topic'].startswith('homeassistant/')}
        paired = configs['homeassistant/device/cleverbox_020000000001_1/config']
        monitored = configs['homeassistant/device/cleverbox_020000000001_2/config']
        assert paired['retain'] and paired['qos'] == 1 and len(configs) == 2
        assert set(paired['payload']['components']) == {'temperature', 'observed_target', 'observed_mode', 'observation_fresh',
                                                        'report_age', 'last_command', 'last_admission'}
        assert set(monitored['payload']['components']) == {'temperature', 'observed_target', 'observed_mode', 'observation_fresh', 'report_age'}
        assert monitored['payload']['device']['name'] == 'Hall'
        temperature = paired['payload']['components']['temperature']
        assert temperature['unique_id'] == 'cleverbox_020000000001_1_temperature'
        assert temperature['state_topic'] == f'{BASE}/radiators/1/state'
        assert paired['payload']['components']['last_admission']['state_topic'] == f'{BASE}/radiators/1/result'
        # Unchanged registry: no republishing. Rename republishes, HA birth replays.
        app.advance(6000)
        assert not any(r['topic'].startswith('homeassistant/device/') for r in drain(app))
        app.http('/api/radiators', {'action': 'rename', 'id': 2, 'name': 'Hallway'})
        app.advance(200)
        assert [r['payload']['device']['name'] for r in drain(app) if r['topic'].endswith('_2/config')] == ['Hallway']
        app.sim('mqtt_message', topic='homeassistant/status', payload='online')
        app.advance(200)
        assert len([r for r in drain(app) if r['topic'].startswith('homeassistant/device/')]) == 2
        # Removal and disable publish empty retained configs.
        app.http('/api/radiators', {'action': 'remove', 'id': 2})
        app.advance(200)
        removed = [r for r in drain(app) if r['topic'].startswith('homeassistant/device/')]
        assert {'topic': 'homeassistant/device/cleverbox_020000000001_2/config', 'payload': '', 'qos': 1, 'retain': True} in removed
        app.http('/api/mqtt', {'action': 'discovery_disable'})
        app.advance(200)
        assert [r['payload'] for r in drain(app) if r['topic'].startswith('homeassistant/device/')] == ['']
        # Disconnected: nothing is published. Disable sends offline first.
        app.sim('mqtt_connection', connected=False)
        app.advance(6000)
        assert drain(app) == []
        app.sim('mqtt_connection', connected=True)
        app.http('/api/mqtt', {'action': 'disable'})
        app.advance(200)
        assert drain(app)[-1] == {'topic': f'{BASE}/availability', 'payload': 'offline', 'qos': 0, 'retain': True}
        assert not app.http('/api/mqtt')['connected']
    finally:
        app.stop()


def main():
    with tempfile.TemporaryDirectory() as directory:
        run(directory)
    print('MQTT: availability, snapshots, commands and rejections, retained commands, discovery publish/rename/birth/removal/disable: passed')


if __name__ == '__main__':
    main()
