#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Pairing, commands and the radiator registry over HTTP, against the simulator.

Expected frames are explicit vectors; nothing here calls the production builders.
"""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
REPORT = '23F1040506C6F1010203DF203000404B02A88A02BA00E100641919DA000A000A0A47CD48C59D'
OFFER = '0FF1010203C6F1100001000003011FF3EA1F'
BIND = '0DF1010203C6F10405060100AA1B1F72'
POLL = '0DF1010203C6F10405061900A01B4377'
# Generated controller state at local time 23:59:59 for each mode.
VECTORS = (
    (2050, 0, '38F1010203C6F10405064500004B02A83B004C00008E000084CC00000000972030049402B100E100641919DA000000005B0078B2173B3B26CE6271'),
    (1550, 3, '38F1010203C6F10405064500004B02A83B004C00008E000084CC000000009720300494025703E100641919DA000000005B0078B2173B3BE9D4B021'),
    (700, 2, '38F1010203C6F10405064500004B02A83B004C00008E000084CC00000000972030049401BE02E100641919DA000000005B0078B2173B3B9450290A'),
    (0, 1, '38F1010203C6F10405064500004B02A83B004C00008E000084CC000000009720300494014001E100641919DA000000005B0078B2173B3B7DE8F50D'),
)
LATE_EVENING = 1767225599  # 2025-12-31 23:59:59 UTC


def crc(data, reflected):
    c = 0xffff
    for b in data:
        c ^= b if reflected else b << 8
        for _ in range(8):
            c = ((c >> 1) ^ (0xa001 if c & 1 else 0)) if reflected else (((c << 1) ^ (0x8005 if c & 0x8000 else 0)) & 0xffff)
    return c


def seal(frame):
    """Test-only CRC writer for modified frames."""
    f = bytearray(frame)
    n = f[0]
    f[n - 1:n + 1] = crc(f[1:n - 1], True).to_bytes(2, 'little')
    f[n + 1:n + 3] = crc(f[:n + 1], False).to_bytes(2, 'big')
    return f.hex().upper()


def changed_frame(hex_frame, offset):
    f = bytearray.fromhex(hex_frame)
    f[offset] ^= 1
    return seal(f)


def report_from(radiator, controller='F1010203', target=None):
    f = bytearray.fromhex(REPORT)
    f[1:5] = bytes.fromhex(radiator)
    f[6:10] = bytes.fromhex(controller)
    if target is not None:
        f[19:21] = (320 + (18 * target + 50) // 100).to_bytes(2, 'big')
    return seal(f)


class App:
    def __init__(self, storage, test_clock=True, real_mqtt=False):
        self.process = subprocess.Popen(
            [sys.executable, str(ROOT / 'host/serve.py'), '--port', '0', '--storage', str(storage)] +
            (['--test-clock'] if test_clock else []) + (['--real-mqtt'] if real_mqtt else []),
            stdout=subprocess.PIPE, text=True)
        line = self.process.stdout.readline().strip()
        assert line.startswith('SIMULATION ONLY: http://127.0.0.1:'), line
        self.url = line.split(' ', 2)[2]

    def stop(self):
        self.process.terminate()
        self.process.wait(timeout=5)
        self.process.stdout.close()

    def http(self, path='/api/radio', data=None, code=200, raw=None, kind='application/json'):
        body = raw if raw is not None else json.dumps(data).encode() if data is not None else None
        request = urllib.request.Request(self.url + path, data=body, headers={'Content-Type': kind})
        try:
            response = urllib.request.urlopen(request, timeout=10)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            text = response.read().decode()
            assert response.status == code, (path, data, response.status, code, text)
            return json.loads(text) if response.headers['Content-Type'] == 'application/json' else text

    def sim(self, action, **fields):
        return self.http('/api/simulation', {'action': action, **fields}, 202)

    def advance(self, ms):
        self.sim('advance', ms=ms)
        return self.http()

    def radiator(self, id):
        return next(d for d in self.http()['radiators'] if d['id'] == id)

    def command(self, id=1, target=2050, mode=0, code=202):
        return self.http('/api/command', {'id': id, 'mode': mode, 'centi_c': target}, code)

    def pair(self, device=0):
        self.sim('device', index=device)
        id = self.http('/api/pairing', {'action': 'start', 'source': 'F1010203', 'destination': 'F1100001'}, 202)['id']
        assert self.advance(2000)['pairing']['phase'] == 'complete'
        return id

    def set_time(self, unix_seconds=LATE_EVENING):
        self.http('/api/time', {'action': 'manual', 'unix_seconds': unix_seconds})


def seed_pairing(storage):
    """Registry v2 with radiator 1: controller F1010203, radiator F1040506."""
    record = (1).to_bytes(4, 'little') + b'Radiator'.ljust(33, b'\0') + bytes.fromhex('F1010203F1040506')
    Path(str(storage) + '.registry').write_bytes(bytes([2, 1, 0, 2, 0, 0, 0]) + record)


def pairing_and_commands(directory):
    storage = Path(directory) / 'pairing'
    app = App(storage)
    try:
        assert app.http('/') == (ROOT / 'firmware/main/index.html').read_text()
        s = app.advance(10000)
        assert s['ready'] and s['simulation_tx'] == [] and s['radiators'] == []
        assert app.http('/api/preview', {'command': 'offer', 'source': 'F1010203', 'destination': 'F1100001'}) == {'frame': OFFER}
        for bad in ({}, {'command': 'state', 'source': 'F1010203', 'destination': 'F1100001'}):
            app.http('/api/preview', bad, 400)
        app.http('/api/pairing', raw=b'{', code=400)
        app.http('/api/pairing', {'action': 'start', 'source': 'F1010203'}, 400)
        app.http('/api/pairing', {'action': 'start', 'source': 'F1010203', 'destination': 'D0100001'}, 400)
        assert app.http()['simulation_tx'] == []
        # Nobody answers an offer from another controller address.
        app.http('/api/pairing', {'action': 'start', 'source': 'F1010204', 'destination': 'F1100001'}, 202)
        assert app.http('/api/pairing', {'action': 'start', 'source': 'F1010203', 'destination': 'F1100001'}, 409)['error'] == 'busy'
        assert app.advance(6000)['pairing']['phase'] == 'timeout'
        assert app.pair() == 2  # ID 1 went to the failed attempt; IDs are never reused.
        s = app.http()
        assert s['simulation_tx'][-2:] == [OFFER, BIND]
        assert s['radiators'] == [{'id': 2, 'name': 'Radiator', 'radiator': 'F1040506', 'paired': True, 'controller': 'F1010203',
                                   'command': {'phase': 'idle', 'attempts': 0, 'error': 0, 'error_name': 'none'}}]
        # Commands need usable device time and valid settings.
        assert app.command(2, code=409)['error'] == 'time_unavailable'
        for bad in ({'id': 2, 'mode': 4, 'centi_c': 2050}, {'id': 2, 'mode': 0, 'centi_c': 400},
                    {'id': 2, 'mode': 0.5, 'centi_c': 2050}, {'mode': 0, 'centi_c': 2050}):
            app.http('/api/command', bad, 400)
        app.http('/api/command', raw=b'{"id":2,"mode":0,"centi_c":2050}', kind='text/plain', code=400)
        assert app.command(9, code=404)['error'] == 'unknown_radiator'
        assert app.http()['simulation_tx'][-1] == BIND
        # Poll, fresh report, generated state frame, then a matching report.
        for target, mode, expected in VECTORS:
            app.set_time()
            app.command(2, target, mode)
            d = app.advance(2)['radiators'][0]
            assert d['command']['phase'] == 'waiting_report' and d['command']['requested_target_c'] == target / 100
            assert app.http()['simulation_tx'][-1] == POLL
            s = app.advance(301)
            assert s['simulation_tx'][-1] == expected and s['radiators'][0]['command']['phase'] == 'waiting_readback'
            assert app.advance(100)['radiators'][0]['command']['phase'] == 'waiting_readback'  # ACK alone is not a match.
            d = app.advance(200)['radiators'][0]
            assert d['command']['phase'] == 'matched' and d['command']['attempts'] == 1
            assert abs(d['observed']['target_c'] - target / 100) < 0.03 and d['observed']['mode'] == mode
        # Reports from another radiator, another controller or with bad CRCs do not update it.
        s = app.http()
        observed, valid, invalid = s['radiators'][0]['observed'], s['valid_frames'], s['invalid_frames']
        for frame in (changed_frame(REPORT, 4), changed_frame(REPORT, 9), REPORT[:-2] + '00'):
            app.sim('inject', frame=frame)
            s = app.advance(5)
            assert s['radiators'][0]['observed']['mode'] == observed['mode']
        assert s['valid_frames'] == valid + 2 and s['invalid_frames'] == invalid + 1
    finally:
        app.stop()
    # Restart: the pairing and next ID persist, commands and reports do not. Nothing is sent at boot.
    app = App(storage)
    try:
        s = app.advance(600000)
        assert s['simulation_tx'] == [] and len(s['radiators']) == 1 and 'observed' not in s['radiators'][0]
        assert s['radiators'][0]['command']['phase'] == 'idle'
        app.set_time(); app.command(2)
        assert app.advance(1000)['radiators'][0]['command']['phase'] == 'matched'
    finally:
        app.stop()


def delivery_rules(directory):
    storage = Path(directory) / 'delivery'
    seed_pairing(storage)
    app = App(storage)
    try:
        app.set_time()
        # Silence: the report wait expires after 150 s without retransmission.
        app.sim('scenario', name='silence')
        app.command()
        assert app.advance(2)['radiators'][0]['command']['phase'] == 'waiting_report'
        assert app.advance(149998)['radiators'][0]['command']['phase'] == 'waiting_report'
        s = app.advance(1)
        assert s['radiators'][0]['command']['phase'] == 'timeout' and s['radiators'][0]['command']['attempts'] == 0
        tx = s['simulation_tx']
        assert app.advance(300000)['simulation_tx'] == tx
        # A later report showing the requested settings changes the result to matched; nothing is sent.
        app.sim('inject', frame=report_from('F1040506', target=2050))
        s = app.advance(5)
        assert s['radiators'][0]['command']['phase'] == 'timeout'  # Never attempted, so not reconciled.
        # ACK without a matching report never counts as applied.
        app.sim('scenario', name='ack_only')
        app.command(target=2200)
        assert app.advance(1000)['radiators'][0]['command']['attempts'] == 1
        assert app.advance(150000)['radiators'][0]['command']['phase'] == 'timeout'
        app.sim('inject', frame=report_from('F1040506', target=2200))
        s = app.advance(5)
        assert s['radiators'][0]['command']['phase'] == 'matched' and s['simulation_tx'][-1] != POLL
        # A fresh mismatching readback allows one retry (two state frames in total).
        app.command(target=2300)
        app.advance(1000)
        app.sim('inject', frame=REPORT)
        assert app.advance(5)['radiators'][0]['command']['attempts'] == 2
        app.sim('inject', frame=REPORT)
        assert app.advance(5)['radiators'][0]['command']['attempts'] == 2
        assert app.advance(150000)['radiators'][0]['command']['phase'] == 'timeout'
        # Delayed replies: progress only when the response arrives.
        app.sim('scenario', name='delayed')
        app.command(target=2400)
        assert app.advance(2000)['radiators'][0]['command']['phase'] == 'waiting_report'
        assert app.advance(1000)['radiators'][0]['command']['phase'] == 'waiting_readback'
        assert app.advance(3000)['radiators'][0]['command']['phase'] == 'matched'
    finally:
        app.stop()


def registry_and_queue(directory):
    storage = Path(directory) / 'registry'
    app = App(storage)
    try:
        assert app.pair(0) == 1 and app.pair(1) == 2
        # A radiator that is already registered fails before bind.
        tx = len(app.http()['simulation_tx'])
        app.sim('device', index=1)
        app.http('/api/pairing', {'action': 'start', 'source': 'F1010203', 'destination': 'F1100001'}, 202)
        s = app.advance(3000)
        assert s['pairing']['phase'] == 'error' and s['pairing']['error_name'] == 'already_registered'
        assert s['simulation_tx'][tx:] == [OFFER] and len(s['radiators']) == 2
        # Commands run one at a time, in order; each radiator has one outstanding command.
        app.set_time()
        app.sim('scenario', name='delayed')
        app.command(1, 2100); app.command(2, 2200)
        assert app.command(2, 2300, code=409)['error'] == 'busy'
        assert app.http('/api/pairing', {'action': 'start', 'source': 'F1010203', 'destination': 'F1100001'}, 409)['error'] == 'busy'
        assert app.http('/api/radiators', {'action': 'remove', 'id': 2}, 409)['error'] == 'busy'
        s = app.advance(2)
        assert [d['command']['phase'] for d in s['radiators']] == ['waiting_report', 'queued']
        s = app.advance(6000)
        assert [d['command']['phase'] for d in s['radiators']] == ['matched', 'waiting_report']
        assert [d['command']['phase'] for d in app.advance(6000)['radiators']] == ['matched', 'matched']
        # A silent radiator holds the queue until its wait expires.
        app.sim('scenario', name='silence_a')
        app.command(1, 2000); app.command(2, 1900)
        assert [d['command']['phase'] for d in app.advance(100000)['radiators']] == ['waiting_report', 'queued']
        assert [d['command']['phase'] for d in app.advance(51000)['radiators']] == ['timeout', 'matched']
        # Rename and remove are saved immediately.
        assert app.http('/api/radiators', {'action': 'rename', 'id': 1, 'name': 'Living room'}) == {'id': 1}
        app.http('/api/radiators', {'action': 'rename', 'id': 1, 'name': ''}, 400)
        app.http('/api/radiators', {'action': 'rename', 'id': 7, 'name': 'X'}, 404)
        assert app.http('/api/radiators', {'action': 'remove', 'id': 2}) == {'id': 2}
        # Monitoring: an observe-only record that accepts reports for any controller.
        app.sim('inject', frame=report_from('F1445566', 'D0123456'))
        s = app.advance(5)
        assert {o['radiator']: o.get('id') for o in s['observations']}['F1445566'] is None
        # ID 3 went to the rejected pairing attempt.
        assert app.http('/api/radiators', {'action': 'monitor', 'radiator': 'F1445566', 'name': 'Hall'}) == {'id': 4}
        assert app.http('/api/radiators', {'action': 'monitor', 'radiator': 'F1445566', 'name': 'Again'}, 409)['error'] == 'already_registered'
        monitored = app.radiator(4)
        assert not monitored['paired'] and 'command' not in monitored and monitored['observed']['controller'] == 'D0123456'
        app.sim('inject', frame=report_from('F1445566', 'F1998877', target=1800))
        app.advance(5)
        monitored = app.radiator(4)
        assert monitored['observed']['controller'] == 'F1998877' and monitored['observed']['target_c'] == 18
        assert app.command(4, code=409)['error'] == 'not_paired'
    finally:
        app.stop()
    app = App(storage)
    try:
        s = app.http()
        assert [(d['id'], d['name'], d['paired']) for d in s['radiators']] == [(1, 'Living room', True), (4, 'Hall', False)]
        assert app.pair(1) == 5 and app.advance(10)['radiators'][-1]['radiator'] == 'F1040507'
    finally:
        app.stop()
    # Corrupt registry: nothing loads, the error is visible and nothing is sent.
    Path(str(storage) + '.registry').write_bytes(b'\x63garbage')
    app = App(storage)
    try:
        s = app.advance(1000)
        assert s['registry_error'] and s['radiators'] == [] and s['simulation_tx'] == []
    finally:
        app.stop()


def main():
    with tempfile.TemporaryDirectory() as directory:
        pairing_and_commands(directory)
        delivery_rules(directory)
        registry_and_queue(directory)
    print('HTTP: pairing, four generated mode vectors, readback, retry/timeout rules, queueing, '
          'monitoring, registry persistence and restart without RF: passed')


if __name__ == '__main__':
    main()
