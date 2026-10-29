#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Production HTTP + network policy/store with injected platform Wi-Fi events."""
from pathlib import Path
import secrets
import tempfile
from test_http import App


def status(app):
    s = app.http('/api/network')
    assert set(s) == {'state', 'setup_result', 'configured', 'recovery_ap', 'attempts', 'ip', 'error'}
    return s


def connect(app):
    app.sim('network_event', name='started')
    app.sim('network_event', name='got_ip')
    return status(app)


def configure(app, ssid, password, code=202):
    response = app.http('/api/network', {'action': 'configure', 'ssid': ssid, 'password': password}, code)
    assert password not in str(response)
    assert ssid not in str(status(app)) and password not in str(status(app))


def failures(app):
    for i in range(3):
        app.sim('network_event', name='started')
        app.sim('network_event', name='disconnected')
        if i < 2:
            assert status(app)['state'] == 'disconnected'
            app.advance(4999)
            assert status(app)['state'] == 'disconnected'
            app.advance(1)
            assert status(app)['state'] == 'connecting'
    s = status(app)
    assert s['state'] == 'failed' and s['recovery_ap'] and s['attempts'] == 3
    return s


def run(directory):
    storage = Path(directory) / 'setup'
    wifi = Path(str(storage) + '.wifi')
    # Ephemeral generated test inputs; no saved household/credential fixtures.
    ssid, password = secrets.token_hex(8), secrets.token_hex(12)
    other_ssid, other_password = secrets.token_hex(8), secrets.token_hex(12)
    app = App(storage)
    try:
        s = status(app)
        assert s['state'] == 'disconnected' and s['recovery_ap'] and not s['configured']
        assert not wifi.exists()
        page = app.http('/')
        assert 'Home Wi-Fi' in page and 'type="password"' in page
        for body in ({}, {'action': 'configure'}, {'action': 'configure', 'ssid': ssid},
                     {'action': 'configure', 'ssid': None, 'password': password},
                     {'action': 'configure', 'ssid': '', 'password': password},
                     {'action': 'configure', 'ssid': 'ü' * 17, 'password': password},
                     {'action': 'configure', 'ssid': ssid, 'password': password[:7]},
                     {'action': 'configure', 'ssid': ssid, 'password': password * 3},
                     {'action': 'configure', 'ssid': ssid, 'password': password + '\n'},
                     {'action': 'configure', 'ssid': ssid + '\0x', 'password': password},
                     {'action': 'forget', 'extra': True}):
            out = app.http('/api/network', body, 400)
            assert ssid not in out and password not in out
        app.http('/api/network', raw=b'{', code=400)
        app.http('/api/network', raw=b'{}', kind='text/plain', code=400)
        app.http('/api/network', raw=b'x' * 768, code=400)
        configure(app, ssid, password)
        assert not wifi.exists() and status(app)['state'] == 'connecting'
        configure(app, other_ssid, other_password, 409)
        # IP before STA_START is stale and cannot persist a new candidate.
        app.sim('network_event', name='got_ip'); assert not wifi.exists()
        s = connect(app)
        assert s['state'] == 'connected' and s['ip'] == '192.0.2.10' and s['configured']
        assert s['setup_result'] == 'saved' and s['recovery_ap']
        saved = wifi.read_bytes()
        app.advance(59999); assert status(app)['recovery_ap']
        app.advance(1); assert not status(app)['recovery_ap']
    finally:
        app.stop()
    app = App(storage)
    try:
        assert status(app)['configured'] and status(app)['state'] == 'connecting'
        assert not app.http()['simulation_tx']
        s = connect(app); assert s['state'] == 'connected' and wifi.read_bytes() == saved
        configure(app, other_ssid, other_password)
        s = failures(app)
        assert s['configured'] and s['setup_result'] == 'failed' and wifi.read_bytes() == saved
        # Previous working setup is automatically retried at the recovery pace.
        app.advance(59999); assert status(app)['state'] == 'failed'
        app.advance(1); assert status(app)['state'] == 'connecting'
        assert connect(app)['state'] == 'connected' and wifi.read_bytes() == saved
        # Pair + save controller state, then admit a command before network loss.
        app.http('/api/pairing', {'action': 'start', 'source': 'F1010203', 'destination': 'F1100001'}, 202)
        app.advance(2000); assert app.http()['pairing']['saved']
        app.sim('fixture'); app.advance(1)
        app.http('/api/controller-state', {'action': 'save'}, 202); app.advance(1)
        registry = Path(str(storage) + '.registry'); before = registry.read_bytes()
        app.sim('scenario', name='delayed')
        app.command(target=2050); app.advance(1)
        app.sim('network_event', name='disconnected')
        assert status(app)['state'] == 'disconnected' and status(app)['recovery_ap']
        assert status(app)['ip'] == ''
        app.advance(4999); assert status(app)['state'] == 'disconnected'
        app.advance(1); assert status(app)['state'] == 'connecting'
        connect(app); app.advance(6000)
        radio = app.http()
        assert radio['command']['phase'] == 'matched'
        tx = radio['simulation_tx']
        # Forget network with a second admitted RF command: no replay or reset.
        app.command(target=2100); app.advance(1)
        app.http('/api/network', {'action': 'forget'}, 202)
        assert not wifi.exists() and not status(app)['configured'] and status(app)['recovery_ap']
        assert registry.read_bytes() == before
        app.advance(11000); assert app.http()['command']['phase'] == 'matched'
        count = len(app.http()['simulation_tx']); assert count > len(tx)
        app.advance(70000); assert len(app.http()['simulation_tx']) == count
        assert status(app)['state'] == 'disconnected'
        app.sim('network_event', name='got_ip'); assert not wifi.exists()
    finally:
        app.stop()
    app = App(storage)
    try:
        assert not status(app)['configured'] and status(app)['recovery_ap']
        assert app.http()['pairing']['saved'] and app.http()['command']['saved_available']
        assert app.http()['simulation_tx'] == []
        configure(app, ssid, password); assert not failures(app)['configured']
        assert not wifi.exists()
        # Explicit setup can recover after failed initial credentials.
        configure(app, ssid, password); assert connect(app)['configured']
    finally:
        app.stop()
    # Malformed/future records are never echoed or silently overwritten at boot.
    wifi.write_bytes(bytes([255]) + secrets.token_bytes(100))
    app = App(storage)
    try:
        assert status(app)['state'] == 'failed' and status(app)['error'] == 'storage_invalid'
        assert status(app)['recovery_ap'] and not status(app)['configured']
        assert app.http()['pairing']['saved']
        app.http('/api/network', {'action': 'forget'}, 202)
        assert not wifi.exists() and registry.read_bytes() == before
        # Real store error path through HTTP: a directory blocks the Wi-Fi key.
        wifi.mkdir()
        (wifi / "blocker").touch()
        configure(app, ssid, password)
        s = connect(app)
        assert s['state'] == 'failed' and s['setup_result'] == 'failed'
        assert s['error'] == 'storage_failed' and s['recovery_ap'] and not s['configured']
        app.http('/api/network', {'action': 'forget'}, 202)
        assert status(app)['error'] == 'storage_failed' and registry.read_bytes() == before
        (wifi / "blocker").unlink()
        wifi.rmdir()
        app.http('/api/network', {'action': 'forget'}, 202)
        assert status(app)['error'] == 'none'
    finally:
        app.stop()


if __name__ == '__main__':
    with tempfile.TemporaryDirectory() as directory:
        run(directory)
    print('Network HTTP validation/privacy, setup/restart, replacement rollback, recovery, RF continuity and network-only forget: passed')
