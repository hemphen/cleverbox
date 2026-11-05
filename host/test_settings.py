#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Wi-Fi, device time and MQTT settings over HTTP, including restarts."""
from pathlib import Path
import tempfile
from test_http import App

WIFI = {'action': 'configure', 'ssid': 'Bench network', 'password': 'synthetic-pass'}
BROKER = {'action': 'configure', 'host': 'broker.test', 'port': 1883, 'username': 'bench', 'password': 'secret', 'enabled': True}


def network(directory):
    storage = Path(directory) / 'network'
    app = App(storage)
    try:
        assert app.http('/api/network') == {'state': 'disconnected', 'ip': '', 'configured': False, 'ap': True, 'error': 0}
        app.http('/api/network', {'action': 'finish_recovery'}, 409)  # Would leave no way in.
        for bad in ({'action': 'configure', 'ssid': '', 'password': 'synthetic-pass'},
                    {'action': 'configure', 'ssid': 'x', 'password': 'short'},
                    {'action': 'configure', 'ssid': 'x' * 33, 'password': 'synthetic-pass'}, {'action': 'other'}):
            app.http('/api/network', bad, 400)
        app.http('/api/network', WIFI)
        s = app.http('/api/network')
        assert s['configured'] and s['state'] == 'connecting' and s['ap'] and 'password' not in s
        app.sim('network_event', name='got_ip')
        assert app.http('/api/network')['state'] == 'connected' and app.http('/api/network')['ip'] == '192.0.2.10'
        app.http('/api/network', {'action': 'finish_recovery'})
        assert not app.http('/api/network')['ap']
        # An ordinary interruption reconnects after five seconds; the AP stays closed.
        app.sim('network_event', name='disconnected')
        assert app.advance(4900) and app.http('/api/network')['state'] == 'disconnected'
        app.advance(200)
        s = app.http('/api/network')
        assert s['state'] == 'connecting' and not s['ap']
        # BOOT: a short press does nothing; five seconds held, then released, opens the AP.
        app.sim('button', low=True); app.advance(1000); app.sim('button', low=False); app.advance(200)
        assert not app.http('/api/network')['ap']
        app.sim('button', low=True); app.advance(5100)
        assert not app.http('/api/network')['ap']  # Opens on release.
        app.sim('button', low=False); app.advance(200)
        assert app.http('/api/network')['ap']
        assert app.http()['simulation_tx'] == []
    finally:
        app.stop()
    app = App(storage)
    try:
        s = app.http('/api/network')
        assert s['configured'] and not s['ap']  # Saved network: station only.
        app.http('/api/network', {'action': 'forget'})
        s = app.http('/api/network')
        assert not s['configured'] and s['ap'] and s['state'] == 'disconnected'
    finally:
        app.stop()
    app = App(storage)
    try:
        assert app.http('/api/network')['ap'] and not app.http('/api/network')['configured']
    finally:
        app.stop()


def device_time(directory):
    storage = Path(directory) / 'time'
    app = App(storage)
    try:
        assert app.http('/api/time') == {'usable': False, 'source': 'unset', 'timezone': 'UTC0'}
        for bad in ({'action': 'manual', 'unix_seconds': 0}, {'action': 'manual', 'unix_seconds': 1767225600.5},
                    {'action': 'timezone', 'timezone': 'Europe/Stockholm'}, {'action': 'other'}):
            app.http('/api/time', bad, 400)
        app.http('/api/time', {'action': 'manual', 'unix_seconds': 1767225600})
        s = app.http('/api/time')
        assert s['usable'] and s['source'] == 'manual' and s['utc'] == '2026-01-01T00:00:00Z'
        app.sim('time_sync', unix_seconds=1780000000)
        assert app.http('/api/time')['source'] == 'network'
        app.http('/api/time', {'action': 'timezone', 'timezone': 'CET-1CEST,M3.5.0/2,M10.5.0/3'})
        assert app.http('/api/time')['local'].endswith('+0200')
    finally:
        app.stop()
    app = App(storage)
    try:  # The zone persists; the clock does not.
        assert app.http('/api/time') == {'usable': False, 'source': 'unset', 'timezone': 'CET-1CEST,M3.5.0/2,M10.5.0/3'}
    finally:
        app.stop()


def mqtt_settings(directory):
    storage = Path(directory) / 'mqtt'
    app = App(storage)
    try:
        s = app.http('/api/mqtt')
        assert not s['configured'] and not s['enabled'] and not s['connected'] and s['bridge_id'] == '020000000001'
        app.http('/api/mqtt', {'action': 'enable'}, 400)  # Nothing configured.
        for bad in ({**BROKER, 'host': 'mqtt://broker'}, {**BROKER, 'port': 0}, {**BROKER, 'enabled': 1},
                    {k: v for k, v in BROKER.items() if k != 'password'}, {'action': 'other'}):
            app.http('/api/mqtt', bad, 400)
        app.http('/api/mqtt', BROKER)
        s = app.http('/api/mqtt')
        assert s['configured'] and s['enabled'] and s['host'] == 'broker.test' and s['username'] == 'bench'
        assert 'secret' not in str(s)
        app.http('/api/mqtt', {'action': 'disable'})
        app.http('/api/mqtt', {'action': 'discovery_enable'})
        s = app.http('/api/mqtt')
        assert not s['enabled'] and s['discovery_enabled']
    finally:
        app.stop()
    app = App(storage)
    try:
        s = app.http('/api/mqtt')
        assert s['configured'] and not s['enabled'] and s['discovery_enabled'] and s['port'] == 1883
        app.http('/api/mqtt', {'action': 'forget'})
        assert not app.http('/api/mqtt')['configured']
    finally:
        app.stop()
    app = App(storage)
    try:
        assert not app.http('/api/mqtt')['configured']
    finally:
        app.stop()


def main():
    with tempfile.TemporaryDirectory() as directory:
        network(directory)
        device_time(directory)
        mqtt_settings(directory)
    print('Settings: Wi-Fi setup, reconnect, BOOT, forget; device time and zone; MQTT settings; persistence across restarts: passed')


if __name__ == '__main__':
    main()
