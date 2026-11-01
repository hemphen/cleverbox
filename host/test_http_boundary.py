#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Physical HTTP branch + production socket classifier, with mapped IPv4 sockets/netifs/RF.
No CLEVERBOX_HOST in http_api.c. Wi-Fi/interface state itself is a host fixture.
"""
import http.client
import json
from pathlib import Path
import tempfile
from urllib.parse import urlsplit
from test_http import seed_pairing, App, STATE, REPORT
from test_network import status, configure, connect
from test_recovery import hold


def request(app, address, path, body=None, expected=200, headers=None, source=None):
    port = urlsplit(app.url).port
    conn = http.client.HTTPConnection(address, port, timeout=5,
                                     source_address=(source, 0) if source else None)
    try:
        conn.request('POST' if body is not None else 'GET', path,
                     json.dumps(body) if body is not None else None,
                     {'Content-Type': 'application/json', **(headers or {})})
        response = conn.getresponse()
        text = response.read().decode()
        assert response.status == expected, (address, path, response.status, text)
        return json.loads(text) if response.getheader('Content-Type') == 'application/json' else text
    finally:
        conn.close()


def files(storage):
    return {p.name: p.read_bytes() for p in storage.parent.glob(storage.name + '*') if p.is_file()}


def run(directory):
    storage = Path(directory) / 'boundary'
    seed_pairing(storage)
    app = App(storage, interfaces=True)
    sta, ap, unknown = '127.0.0.1', '127.0.0.2', '127.0.0.3'
    spoof = {'Host': '192.0.2.10', 'Origin': 'http://192.0.2.10',
             'Forwarded': 'for=192.0.2.20;host=192.0.2.10',
             'X-Forwarded-For': '192.0.2.20', 'X-Forwarded-Host': '192.0.2.10',
             'X-Interface': 'sta'}
    try:
        app.advance(1)
        for destination, name in ((sta, 'sta'), (ap, 'ap'), (unknown, 'unknown')):
            s = request(app, destination, '/api/network', headers=spoof)
            assert s['http_interface'] == name and s['http_control_allowed'] == (name == 'sta')
            for path in ('/api/radio', '/api/radiators', '/api/time', '/api/mqtt', '/api/commissioning'):
                request(app, destination, path, headers=spoof)
        # Unknown classification never authorises connectivity mutations either.
        request(app, unknown, '/api/network', {'action': 'forget', 'confirm': True}, 403, spoof)
        # Actual socket peer guard: an AP peer cannot get LAN rights by targeting STA IP.
        request(app, sta, '/api/pairing', {'action': 'cancel', 'id': 1}, 403, spoof, source=ap)
        assert request(app, sta, '/api/network', source=ap)['http_interface'] == 'unknown'
        # AP can restore Wi-Fi. Both interfaces remain independently classified.
        request(app, ap, '/api/network', {'action': 'configure', 'ssid': 'generated', 'password': 'testonly123'}, 202)
        connect(app)
        assert status(app)['configured']
        assert 'template_available' not in app.http()['command']
        app.advance(1)
        actions = [('/api/pairing', {'action': a, 'source': 'F1010203', 'destination': 'F1100001', 'id': 1})
                   for a in ('start', 'save', 'cancel')]
        actions += [('/api/radiators', {'action': a, 'id': 1, 'name': 'Blocked'}) for a in ('rename', 'remove', 'select')]
        actions += [('/api/commissioning', {'mode':'control-enabled'})]
        actions += [('/api/command', {'id': 1, 'mode': 0, 'centi_c': 2050, 'state': STATE}),
                    ('/api/preview', {'command': 'offer', 'source': 'F1010203', 'destination': 'F1100001'}),
                    ('/api/time', {'action': 'manual', 'unix_seconds': 1767268800}),
                    ('/api/time', {'action': 'timezone', 'timezone': 'CET-1'}),
                    ('/api/mqtt', dict(action='configure', host='broker.test', port=1883, username='', password='', enabled=True))]
        actions += [('/api/mqtt', {'action': a}) for a in ('enable', 'disable', 'forget', 'discovery_enable', 'discovery_disable')]
        before = files(storage)
        for destination in (ap, unknown):
            for path, body in actions:
                request(app, destination, path, body, 403, spoof)
                s = app.http()
                assert not s['mutation_pending'] and not s['exchange_id'] and not s['active_commands']
        app.advance(1000)
        assert files(storage) == before and not app.http('/api/simulation/radio')['simulation_tx']
        assert app.http()['command']['phase'] == 'idle'
        # This test must be compiled against the physical branch: even a valid
        # override cannot bypass unsynchronised time (nor be silently ignored).
        for value in (0, 86399, None, '0', {}, -1):
            text = request(app, sta, '/api/command', {'mode': 0, 'centi_c': 2050, 'clock_seconds': value}, 400)
            assert 'host-only' in text
        text = app.http('/api/command', {'mode': 0, 'centi_c': 2050}, 409)
        assert 'time unavailable' in text
        assert not app.http('/api/simulation/radio')['simulation_tx'] and files(storage) == before
        app.http('/api/time', {'action': 'manual', 'unix_seconds': 1767268800}, 200)
        app.http('/api/time', {'action': 'timezone', 'timezone': 'CET-1'}, 200)
        request(app, sta, '/api/command', {'mode': 0, 'centi_c': 2050, 'clock_seconds': 0}, 400)
        app.sim('scenario', name='silence')
        app.http('/api/command', {'mode': 0, 'centi_c': 2050}, 202)
        app.advance(1)
        tx = app.http('/api/simulation/radio')['simulation_tx']
        assert len(tx) == 1 and app.http()['command']['phase'] == 'poll'
        app.advance(1)  # Owner resolves TX on a later iteration.
        assert app.http()['command']['phase'] == 'waiting_report'
        app.advance(41); hold(app)
        app.sim('network_event', name='disconnected')
        assert status(app)['manual_recovery'] and status(app)['recovery_ap']
        request(app, ap, '/api/command', {'mode': 1, 'centi_c': 0}, 403, spoof)
        # Finish while disconnected closes predictably, never starts new work.
        request(app, ap, '/api/network', {'action': 'finish_recovery'}, 202)
        app.advance(59999); assert status(app)['recovery_ap'] and status(app)['ap_closing']
        app.advance(1); assert not status(app)['recovery_ap']
        assert app.http('/api/simulation/radio')['simulation_tx'] == tx
        app.sim('scenario', name='normal'); app.sim('inject', frame=REPORT); app.advance(1000)
        assert app.http()['command']['phase'] == 'matched'
        tx = app.http('/api/simulation/radio')['simulation_tx']
        assert len(tx) == 2
        # Captured device-managed local clock: 13:00 plus elapsed whole seconds.
        sent = bytes.fromhex(tx[-1])
        assert sent[51] == 0xB2 and sent[52] == 13
        app.advance(60000); assert app.http('/api/simulation/radio')['simulation_tx'] == tx
        # AP diagnostic responses never return credentials.
        assert 'testonly123' not in str(request(app, ap, '/api/network'))
        request(app, ap, '/api/network', {'action': 'forget', 'confirm': True}, 202)
        assert not status(app)['configured'] and status(app)['recovery_ap']
        assert app.http()['pairing']['saved'] and 'template_available' not in app.http()['command']
    finally:
        app.stop()


if __name__ == '__main__':
    with tempfile.TemporaryDirectory() as directory:
        run(directory)
    print('Physical HTTP branch: socket AP/STA/unknown admission, spoofing, no mutation/TX, clock gate and RF continuity: passed')
