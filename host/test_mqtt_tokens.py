#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Focused single-use command capability regressions on production routing/RF."""
import json
from pathlib import Path
import tempfile
from test_http import App, STATE
from test_mqtt import BASE, connect, publications, send


def run(directory):
    storage = Path(directory) / 'tokens.bin'
    storage.write_bytes(bytes.fromhex('01F1010203F1040506'))
    app = App(storage)
    try:
        app.http('/api/mqtt', dict(action='configure', host='broker.test', port=1883,
                                  username='', password='', enabled=True), 202)
        app.http('/api/network', dict(action='configure', ssid='synthetic', password='synthetic'), 202)
        app.sim('network_event', name='started'); app.sim('network_event', name='got_ip'); app.advance(100)
        session = connect(app)
        def snapshot():
            app.advance(5000)
            return [r['payload'] for r in publications(app) if r['topic'].endswith('/1/state')][-1]
        def command(token, **fields):
            body = dict(session=session, command_token=token, mode=0, centi_c=2050)
            body.update(fields)
            return send(app, session, 0, payload=json.dumps(body))
        token = snapshot()['command_token']
        assert len(token) == 32 and snapshot()['command_token'] == token
        assert command(token, session='old')['reason'] == 'session_mismatch'
        assert command(token, command_token=None)['reason'] == 'invalid_fields'
        assert command(token, request_id=10)['reason'] == 'invalid_fields'
        # Failed prerequisites consume the capability, not just successful RF.
        assert command(token)['reason'] == 'time_unavailable'
        assert command(token)['reason'] == 'invalid_command_token'
        token = snapshot()['command_token']
        app.http('/api/time', dict(action='manual', unix_seconds=1767268800))
        assert command(token)['reason'] == 'busy_or_unavailable_or_missing_template'
        assert command(token)['reason'] == 'invalid_command_token'
        token = snapshot()['command_token']
        app.http('/api/controller-state', dict(action='import', id=1, state=STATE), 202); app.advance(1)
        # Numeric clients and token clients share ONE atomic sequence. Floor is
        # never an allocator: a token allocates at consumption, after numeric 100.
        assert send(app, session, 100)['accepted']; app.advance(1000)
        assert command(token)['reason'] == 'invalid_command_token'
        token = snapshot()['command_token']
        app.sim('scenario', name='silence')
        accepted = command(token)
        assert accepted['accepted'] and accepted['request_id'] == 101 and accepted['command_token'] == token
        tx = app.http()['simulation_tx']
        assert command(token)['reason'] == 'invalid_command_token'
        assert app.http()['simulation_tx'] == tx
        token2 = snapshot()['command_token']
        busy = command(token2)
        assert busy['request_id'] == 102 and busy['reason'] == 'busy_or_unavailable_or_missing_template'
        assert command(token2)['reason'] == 'invalid_command_token'
        app.advance(151000)
        delivery = [r['payload'] for r in publications(app) if r['payload'].get('stage') == 'delivery'][-1]
        assert delivery['phase'] == 'timeout' and delivery['request_id'] == 101 and delivery['command_token'] == token
        token3 = snapshot()['command_token']
        # Invalid complete payloads consume too. The token cannot be repaired/replayed.
        assert command(token3, centi_c=3500)['reason'] == 'invalid_fields'
        assert command(token3)['reason'] == 'invalid_command_token'
        # Retained input must not consume a still valid permit.
        token4 = snapshot()['command_token']
        payload = json.dumps(dict(session=session, command_token=token4, mode=0, centi_c=2050))
        assert send(app, session, 0, payload=payload, retained=True)['reason'] == 'retained_command'
        app.sim('scenario', name='normal')
        assert command(token4)['accepted']; app.advance(1000)
        assert app.http()['command']['phase'] == 'matched'
        # Old session and token remain unusable after reconnect; no RF on reconnect.
        tx = app.http()['simulation_tx']
        app.sim('mqtt_connection', connected=False); app.advance(5100)
        old_session = session; session = connect(app)
        assert session != old_session and app.http()['simulation_tx'] == tx
        assert command(token4, session=old_session)['reason'] == 'session_mismatch'
        assert command(token4)['reason'] == 'invalid_command_token'
        # Sequence exhaustion fails closed instead of wrapping/losing precision.
        assert send(app, session, 9007199254740991)['accepted']; app.advance(1000)
        assert command(snapshot()['command_token'])['reason'] == 'request_id_exhausted'
    finally:
        app.stop()


if __name__ == '__main__':
    with tempfile.TemporaryDirectory() as directory:
        run(directory)
    print('Single-use MQTT tokens: allocation, mixed clients, rejection consumption, replay, correlation, reconnect and exhaustion: passed')
