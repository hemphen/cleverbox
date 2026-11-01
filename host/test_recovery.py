#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Production button/policy/HTTP/RF with fake GPIO levels and Wi-Fi, never physical I/O."""
from pathlib import Path
import secrets
import tempfile
from test_http import App
from test_network import status, configure, connect
from test_mqtt import connect as mqtt_connect, publications


def hold(app, ms=5000):
    was_manual = status(app)['manual_recovery']
    app.sim('button_level', low=True)
    app.advance(1)  # First sampled low, not a guessed edge timestamp.
    app.advance(ms - 1)
    app.sim('button_level', low=False)
    app.advance(40)
    assert status(app)['manual_recovery'] == was_manual
    app.advance(1)  # 40 ms stable release after its first sample.


def finish(app):
    app.http('/api/network', {'action': 'finish_recovery'}, 202)
    assert not status(app)['manual_recovery']


def run(directory):
    storage = Path(directory) / 'recovery'
    ssid, password = secrets.token_hex(8), secrets.token_hex(16)
    broker_password = secrets.token_hex(16)
    app = App(storage)
    try:
        # Startup-held cannot arm, even after a very long low. A release arms only.
        app.sim('button_level', low=True); app.advance(10000)
        assert not status(app)['manual_recovery']
        app.sim('button_level', low=False); app.advance(41)
        assert not status(app)['manual_recovery']
        hold(app, 4999); assert not status(app)['manual_recovery']
        hold(app); assert status(app)['manual_recovery'] and status(app)['recovery_ap']
        finish(app); app.advance(70000)
        assert not status(app)['recovery_ap'] and not status(app)['configured']
        # Explicit confirmation is required at the HTTP boundary, with exact schema.
        for body in ({'action': 'forget'}, {'action': 'forget', 'confirm': False},
                     {'action': 'forget', 'confirm': 1}, {'action': 'forget', 'confirm': 'true'},
                     {'action': 'finish_recovery', 'extra': True}):
            app.http('/api/network', body, 400)
        configure(app, ssid, password)
        hold(app); assert status(app)['manual_recovery'] and status(app)['setup_result'] == 'pending'
        connect(app); assert not status(app)['manual_recovery']
        app.advance(60000); assert not status(app)['recovery_ap']
        hold(app); assert status(app)['manual_recovery'] and status(app)['state'] == 'connected'
        app.sim('network_event', name='got_ip'); app.advance(120000)
        assert status(app)['manual_recovery'] and status(app)['recovery_ap']
        finish(app); app.advance(59999); assert status(app)['recovery_ap']
        app.advance(1); assert not status(app)['recovery_ap']
        # Stable IDs, names, pairings, timezone, broker credentials and
        # a durable offline discovery cleanup record must survive recovery/forget.
        app.http('/api/pairing', {'action': 'start', 'source': 'F1010203', 'destination': 'F1100001'}, 202)
        app.advance(2000)
        app.http('/api/radiators', {'action': 'rename', 'id': 1, 'name': 'Bench'}, 202); app.advance(1)
        app.http('/api/time', {'action': 'timezone', 'timezone': 'CET-1'}, 200)
        app.http('/api/mqtt', dict(action='configure', host='broker.test', port=1883,
                                 username='bench', password=broker_password, enabled=True), 202)
        app.advance(100); session = mqtt_connect(app)
        hold(app)
        assert app.http('/api/mqtt')['connected'] and app.http('/api/mqtt')['session'] == session
        assert status(app)['state'] == 'connected'  # Runtime recovery leaves STA/MQTT live.
        app.http('/api/mqtt', {'action': 'discovery_enable'}, 202); app.advance(1000)
        publications(app)
        # Disconnect + disable discovery while offline leaves durable cleanup.
        app.sim('mqtt_connection', connected=False); app.advance(100)
        app.http('/api/mqtt', {'action': 'discovery_disable'}, 202); app.advance(100)
        assert app.http('/api/mqtt')['discovery_pending'] > 0
        unrelated = {suffix: Path(str(storage) + suffix).read_bytes()
                     for suffix in ('.registry', '.timezone', '.mqtt', '.ha_discovery')}
        wifi = Path(str(storage) + '.wifi'); saved_wifi = wifi.read_bytes()
        tx = len(app.http()['simulation_tx'])
        hold(app); app.advance(10000)
        assert len(app.http()['simulation_tx']) == tx
        assert app.http()['inclusion']['phase'] == 'complete'
        # A silent A command remains admitted while recovery and DHCP run. Release
        # normal replies afterwards: its single poll is followed by one state TX.
        app.sim('scenario', name='silence'); app.command(); app.advance(2)
        assert app.http()['command']['phase'] == 'waiting_report'
        poll_count = len(app.http()['simulation_tx']); hold(app)
        assert len(app.http()['simulation_tx']) == poll_count
        app.sim('network_event', name='disconnected')
        assert status(app)['manual_recovery'] and status(app)['recovery_ap']
        finish(app)  # Explicit completion also closes when disconnected.
        app.advance(5000); connect(app)
        hold(app); assert status(app)['manual_recovery']
        # RF continuity uses an independently fixed report from existing tests.
        from test_http import REPORT
        app.sim('scenario', name='normal'); app.sim('inject', frame=REPORT); app.advance(1000)
        assert app.http()['command']['phase'] == 'matched'
        assert len(app.http()['simulation_tx']) == poll_count + 1
        # Failed saves and forgets cannot drop manual access or hide uncertainty.
        app.sim('wifi_storage_fault', mode=1)
        configure(app, secrets.token_hex(8), secrets.token_hex(16)); connect(app)
        assert status(app)['manual_recovery'] and status(app)['error'] == 'storage_failed'
        assert wifi.read_bytes() == saved_wifi
        app.sim('wifi_storage_fault', mode=2)
        app.http('/api/network', {'action': 'forget', 'confirm': True}, 202)
        assert status(app)['manual_recovery'] and status(app)['persistence_uncertain']
        finish(app); app.advance(60000); connect(app); app.advance(60000)
        assert status(app)['persistence_uncertain'] and not status(app)['recovery_ap']
        # Successful explicit save resolves uncertainty and releases recovery.
        app.sim('wifi_storage_fault', mode=0)
        configure(app, ssid, password); connect(app)
        assert not status(app)['manual_recovery'] and not status(app)['persistence_uncertain']
        app.http('/api/network', {'action': 'forget', 'confirm': True}, 202)
        assert not wifi.exists() and status(app)['recovery_ap']
        for suffix, data in unrelated.items():
            assert Path(str(storage) + suffix).read_bytes() == data
        # Restart during admitted delivery restores config only, no pending work.
        app.sim('scenario', name='silence'); app.command(target=2100); app.advance(2)
        assert app.http()['command']['phase'] == 'waiting_report'
        hold(app); assert status(app)['manual_recovery']
    finally:
        app.stop()
    app = App(storage)
    try:
        s = status(app)
        assert not s['manual_recovery'] and not s['configured'] and s['recovery_ap']
        radio = app.advance(1000)
        assert radio['command']['phase'] == 'idle' and not radio['simulation_tx']
        assert radio['pairing']['saved']
        assert radio['radiators'][0]['name'] == 'Bench'
        assert app.http('/api/time')['timezone'] == 'CET-1'
        assert app.http('/api/mqtt')['configured'] and app.http('/api/mqtt')['discovery_pending'] > 0
        for suffix, data in unrelated.items():
            assert Path(str(storage) + suffix).read_bytes() == data
        assert password not in str(s) and broker_password not in str(app.http('/api/mqtt'))
    finally:
        app.stop()


if __name__ == '__main__':
    with tempfile.TemporaryDirectory() as directory:
        run(directory)
    print('Recovery HTTP/button timing, latch, confirmed forget, failure/uncertainty, RF continuity and restart isolation: passed')
