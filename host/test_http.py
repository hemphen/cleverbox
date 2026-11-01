#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Black-box HTTP tests; expected exchange vectors do not use production builders."""
import json
from pathlib import Path
import subprocess
import socket
import sys
import tempfile
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
STATE = '38F1010203C6F10405064500004B02A83B004C00008E000084CC00000000972030049402BA00E100641919DA000000005B0078B20C0000A20F60D9'
REPORT = '23F1040506C6F1010203DF203000404B02A88A02BA00E100641919DA000A000A0A47CD48C59D'
OFFER = '0FF1010203C6F1100001000003011FF3EA1F'
POLL = '0DF1010203C6F10405061900A01B4377'
BIND = '0DF1010203C6F10405060100AA1B1F72'
# From independently assembled/sealed test_control.c vectors (pinned reference CRC).
COMFORT = '38F1010203C6F10405064500004B02A83B004C00008E000084CC00000000972030049402B100E100641919DA000000005B0078B2173B3B26CE6271'
ECO = '38F1010203C6F10405064500004B02A83B004C00008E000084CC000000009720300494025703E100641919DA000000005B0078B2173B3BE9D4B021'


def changed_frame(hex_frame, offset):
    """Test-only byte mutation with fresh CRCs, not a packet oracle."""
    f = bytearray.fromhex(hex_frame)
    f[offset] ^= 1
    def crc(data, reflected):
        c = 0xffff
        for b in data:
            c ^= b if reflected else b << 8
            for _ in range(8):
                c = ((c >> 1) ^ (0xa001 if c & 1 else 0)) if reflected else (((c << 1) ^ (0x8005 if c & 0x8000 else 0)) & 0xffff)
        return c
    l = f[0]
    f[l-1:l+1] = crc(f[1:l-1], True).to_bytes(2, 'little')
    f[l+1:l+3] = crc(f[:l+1], False).to_bytes(2, 'big')
    return f.hex()


class App:
    def __init__(self, storage, test_clock=True, real_mqtt=False, interfaces=False, operating_mode="control-enabled", mode_storage_fault=0, radio_init_fault=0):
        # Existing control suites explicitly provision their operating-mode fixture.
        # Passive/fresh/migration tests pass None and exercise production defaults.
        mode_file = Path(str(storage) + '.rf_mode')
        if operating_mode is not None and not mode_file.exists():
            mode_file.write_bytes(bytes([1, operating_mode == 'control-enabled']))
        self.process = subprocess.Popen([sys.executable, str(ROOT / 'host/serve.py'), '--port', '0', '--storage', str(storage)] + (['--test-clock'] if test_clock else []) + (['--real-mqtt'] if real_mqtt else []) + (['--test-interfaces'] if interfaces else []) + (['--mode-storage-fault', str(mode_storage_fault)] if mode_storage_fault else []) + (['--radio-init-fault', str(radio_init_fault)] if radio_init_fault else []), stdout=subprocess.PIPE, text=True)
        line = self.process.stdout.readline().strip()
        assert line.startswith('SIMULATION ONLY: http://127.0.0.1:'), line
        self.url = line.split(' ', 2)[2]

    def stop(self):
        self.process.terminate()
        self.process.wait(timeout=5)
        self.process.stdout.close()

    def http(self, path='/api/radio', data=None, code=200, raw=None, kind='application/json'):
        body = raw if raw is not None else json.dumps(data).encode() if data is not None else None
        req = urllib.request.Request(self.url + path, data=body, headers={'Content-Type': kind})
        try:
            response = urllib.request.urlopen(req, timeout=5)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            text = response.read().decode()
            assert response.status == code, (response.status, code, text)
            assert response.headers['Cache-Control'] == 'no-store'
            return json.loads(text) if response.headers['Content-Type'] == 'application/json' else text

    def sim(self, action, **kw):
        return self.http('/api/simulation', {'action': action, **kw}, 202)

    def advance(self, ms):
        self.sim('advance', ms=ms)
        return self.http()

    def command(self, target=2050, mode=0, **kw):
        return self.http('/api/command', {'mode': mode, 'centi_c': target, 'clock_seconds': 86399, **kw}, 202)


def saved_state_workflow(directory):
    storage = Path(directory) / 'saved-pair.bin'
    legacy_pair = bytes.fromhex('01F1010203F1040506')
    storage.write_bytes(legacy_pair)  # Exact existing pairing-only v1 format.
    state_file = Path(str(storage) + '.registry')
    app = App(storage)
    try:
        s = app.advance(1000)
        assert s['pairing']['saved'] and s['simulation_tx'] == []
        assert s['command']['template_source'] == 'missing'
        assert s['command']['frame_source'] == 'generated-touch-e3'
        app.http('/api/controller-state', {'action': 'save'}, 409)
        for data in ({}, {'action': 'import'}, {'action': 'import', 'state': None}):
            app.http('/api/controller-state', data, 400)
        app.sim('fixture')
        app.advance(1)
        app.http('/api/controller-state', {'action': 'save'}, 202)
        s = app.advance(1)
        assert s['command']['save_status'] == 'succeeded'
        assert s['command']['template_source'] == 'learned' and s['command']['saved_available']
        assert 'observed' not in s and s['simulation_tx'] == []
        saved = state_file.read_bytes()
        assert saved[0] == 1 and int.from_bytes(saved[1:3], 'little') == 1 and saved.endswith(bytes.fromhex(STATE))
        assert storage.read_bytes() == legacy_pair
        # Passive learning cannot write flash; every invalid import preserves it.
        newer = changed_frame(STATE, 49)
        app.sim('inject', frame=newer)
        app.advance(1)
        assert state_file.read_bytes() == saved
        for frame in ('zz', STATE[:-2], STATE[:-2] + '00', changed_frame(STATE, 30),
                      changed_frame(STATE, 9), REPORT):
            app.http('/api/controller-state', {'action': 'import', 'state': frame}, 400)
            assert state_file.read_bytes() == saved
        assert app.http()['simulation_tx'] == []
    finally:
        app.stop()
    app = App(storage)
    try:
        s = app.advance(1000)
        c = s['command']
        assert c['template_source'] == 'saved' and c['template_available']
        assert not c['learned_available'] and 'template_age_ms' not in c
        assert c['save_status'] == 'not_attempted' and c['phase'] == 'idle'
        assert 'observed' not in s and s['simulation_tx'] == []
        app.sim('scenario', name='silence')
        app.command()
        s = app.advance(100)
        assert s['command']['phase'] == 'waiting_report' and s['simulation_tx'] == [POLL]
        app.sim('inject', frame=REPORT)
        s = app.advance(2)
        assert s['command']['phase'] == 'waiting_readback' and s['simulation_tx'][-1] == COMFORT
        assert app.advance(100)['command']['phase'] == 'waiting_readback'
        matching = '23F1040506C6F1010203DF203000404B02A88A02B100E100641919DA000A000A0A47C783C11D'
        app.sim('inject', frame=matching)
        assert app.advance(1)['command']['phase'] == 'matched'
        # Per-command overrides are explicitly rejected. Learning different legacy
        # bytes during delivery cannot change the generated profile or saved data.
        override = changed_frame(changed_frame(STATE, 32), 4)
        error = app.http('/api/command', {'mode':0,'centi_c':2050,'state':override}, 400)
        assert 'deprecated' in error
        app.command()
        app.advance(2)  # TX completion runs on the following owner tick.
        newer = changed_frame(STATE, 49)
        app.sim('inject', frame=newer)
        app.advance(1)
        app.sim('inject', frame=REPORT)
        s = app.advance(2)
        sent = bytes.fromhex(s['simulation_tx'][-1])
        assert sent[31:34] == bytes.fromhex('203004')
        assert sent[49:51] == bytes.fromhex('0078')
        assert sent[1:5] == bytes.fromhex('F1010203')  # Manual source behavior retained.
        assert state_file.read_bytes() == saved
        app.sim('inject', frame=matching)
        app.advance(1)
        # Later commands also ignore learned RAM, without overwriting saved data.
        app.command()
        app.advance(2)  # TX completion runs on the following owner tick.
        app.sim('inject', frame=REPORT)
        s = app.advance(2)
        assert bytes.fromhex(s['simulation_tx'][-1])[49:51] == bytes.fromhex('0078')
        assert state_file.read_bytes() == saved
        assert s['command']['phase'] == 'waiting_readback'
    finally:
        app.stop()  # Restart during delivery must not replay it.
    app = App(storage)
    try:
        s = app.advance(600000)
        assert s['command']['phase'] == 'idle' and s['command']['template_source'] == 'saved'
        assert s['simulation_tx'] == [] and 'observed' not in s
        # Import-and-save accepts the historic manual source policy, without RX/TX.
        imported = changed_frame(STATE, 4)  # Unusual source and every opaque record.
        for offset in (11,17,20,22,24,27,32,33,49,50):
            imported = changed_frame(imported, offset)
        app.http('/api/controller-state', {'action': 'import', 'state': imported}, 202)
        s = app.advance(1)
        assert s['command']['save_status'] == 'succeeded' and s['simulation_tx'] == []
        assert not s['command']['learned_available'] and 'observed' not in s
        assert state_file.read_bytes().endswith(bytes.fromhex(imported))
    finally:
        app.stop()
    app = App(storage)
    try:
        assert app.http()['command']['template_source'] == 'saved'
        assert app.advance(600000)['simulation_tx'] == []  # Old legacy data grants no work.
        app.sim('scenario', name='silence')
        app.command()
        app.advance(2)  # TX completion runs on the following owner tick.
        app.sim('inject', frame=REPORT)
        s = app.advance(2)
        assert s['simulation_tx'][-1] == COMFORT
        assert bytes.fromhex(s['simulation_tx'][-1])[31:34] == bytes.fromhex('203004')
        assert bytes.fromhex(s['simulation_tx'][-1])[1:5] == bytes.fromhex('F1010203')
        app.sim('fixture')
        app.advance(1)
        tx = app.http()['simulation_tx']
        app.http('/api/controller-state', {'action': 'clear'}, 202)
        s = app.advance(1)
        assert s['command']['clear_status'] == 'succeeded'
        assert s['command']['template_source'] == 'missing' and s['pairing']['saved']
        assert s['command']['phase'] == 'waiting_readback'  # Active snapshot unchanged.
        assert s['simulation_tx'] == tx and storage.read_bytes() == legacy_pair
    finally:
        app.stop()
    app = App(storage)
    try:
        s = app.advance(1000)
        assert s['pairing']['saved'] and s['command']['template_source'] == 'missing'
        assert s['simulation_tx'] == [] and s['command']['phase'] == 'idle'
        app.command()
        assert app.advance(1000)['command']['phase'] == 'matched'
    finally:
        app.stop()
    # Unknown, malformed, CRC-corrupt and differently paired state remain visible
    # on disk. The separate pairing record still loads and is never rewritten.
    legacy_saved=legacy_pair + bytes.fromhex(STATE)
    for bad in (b'\x63' + legacy_saved[1:], legacy_saved[:-1], legacy_saved[:-1] + bytes([legacy_saved[-1] ^ 1]),
                legacy_saved[:4] + bytes([legacy_saved[4] ^ 1]) + legacy_saved[5:]):
        Path(str(storage)+'.registry').unlink(missing_ok=True)
        state_file=Path(str(storage)+'.state')
        state_file.write_bytes(bad)
        app = App(storage)
        try:
            s = app.advance(1000)
            assert s['pairing']['saved'] and s['command']['state_load_error']
            assert s['command']['template_source'] == 'missing' and s['simulation_tx'] == []
            assert storage.read_bytes() == legacy_pair and state_file.read_bytes() == bad
        finally:
            app.stop()
    Path(str(storage)+'.registry').unlink(missing_ok=True)
    storage.write_bytes(b'\x63' + legacy_pair[1:])
    app = App(storage)
    try:
        s = app.advance(1000)
        assert not s['pairing']['saved'] and s['pairing']['error'] and s['simulation_tx'] == []
        app.http('/api/pairing', {'action': 'start', 'source': 'F1010203', 'destination': 'F1100001'}, 409)
        assert storage.read_bytes() == b'\x63' + legacy_pair[1:]
    finally:
        app.stop()


def generated_retry_context(directory):
    storage = Path(directory) / 'generated-context.bin'
    storage.write_bytes(bytes.fromhex('01F1010203F1040506'))
    app = App(storage)
    try:
        app.sim('scenario', name='silence')
        # A cached report, even one received this boot, cannot answer a later poll.
        app.sim('inject', frame=REPORT); app.advance(1)
        app.advance(300000)
        assert app.http()['observed']['stale']
        app.command()
        app.advance(2)
        assert app.http()['command']['phase'] == 'waiting_report'
        baseline = app.http()['simulation_tx']
        # Bad CRC, other radiator, other controller and incompatible 0A context.
        for frame in (REPORT[:-2]+'00', changed_frame(REPORT,4),
                      changed_frame(REPORT,9), changed_frame(REPORT,33)):
            app.sim('inject',frame=frame); app.advance(1)
            assert app.http()['command']['phase']=='waiting_report'
            assert app.http()['simulation_tx']==baseline
        app.sim('inject',frame=REPORT); app.advance(2)
        first = bytes.fromhex(app.http()['simulation_tx'][-1])
        assert app.http()['command']['phase']=='waiting_readback'
        # Retry uses new report echoes, preserving clock origin and exact deadline.
        app.advance(2000)
        newer = changed_frame(changed_frame(changed_frame(REPORT,16),13),24)
        app.sim('inject',frame=newer); app.advance(2)
        status = app.http(); second = bytes.fromhex(status['simulation_tx'][-1])
        raw = bytes.fromhex(newer)
        assert second[14:16] == raw[16:18] and second[39:43] == raw[23:27]
        assert second[31:34] == bytes([raw[11],0x30,4|(raw[13]&1)])
        assert second[49:51] == bytes.fromhex('0078')
        assert first[52:55] == bytes([23,59,59]) and second[52:55] == bytes([0,0,1])
        assert status['command']['attempts']==2 and status['command']['phase']=='waiting_readback'
        tx = status['simulation_tx']
        app.sim('inject',frame=newer); app.advance(2)
        app.advance(149997)
        assert app.http()['command']['phase']=='waiting_readback'
        app.advance(1)
        assert app.http()['command']['phase']=='timeout' and app.http()['simulation_tx']==tx
    finally:
        app.stop()


def main():
    with tempfile.TemporaryDirectory() as directory:
        saved_state_workflow(directory)
        generated_retry_context(directory)
        storage = Path(directory) / 'pairing.bin'
        app = App(storage)
        try:
            page = app.http('/')
            assert page == (ROOT / 'firmware/main/index.html').read_text()
            assert 'SIMULATION' in page and 'synthetic controller-state fixture' in page
            s = app.advance(10000)
            assert s['simulation'] and s['simulation_tx'] == []
            assert not s['pairing']['saved'] and s['command']['phase'] == 'idle'
            # Real handlers: strict type/length/integer/import validation and previews.
            for data in ({}, {'mode': 0.1, 'centi_c': 2050, 'clock_seconds': 0}, {'mode': 0, 'centi_c': 2050, 'clock_seconds': 0, 'state': None}):
                app.http('/api/command', data, 400)
            app.http('/api/pairing', raw=b'{', code=400)
            app.http('/api/command', raw=b'{}', kind='text/plain', code=400)
            app.http('/api/command', raw=b'x' * 512, code=400)
            assert app.http('/api/preview', {'command': 'offer', 'source': 'F1010203', 'destination': 'F1100001'}) == {'status': 'preview', 'transmitted': False, 'frame': OFFER}
            assert app.http()['simulation_tx'] == []
            # Wrong source/payload TX must not elicit a simulated success.
            app.http('/api/pairing', {'action': 'start', 'source': 'F1010204', 'destination': 'F1100001'}, 202)
            assert app.advance(6000)['pairing']['phase'] == 'timeout'
            app.http('/api/pairing', {'action': 'start', 'source': 'F1010203', 'destination': 'F1100001'}, 202)
            s = app.advance(2000)
            assert s['pairing']['saved'] and s['pairing']['radiator'] == 'F1040506'
            assert s['simulation_tx'][-2:] == [OFFER, BIND]
            assert Path(str(storage)+'.registry').exists()
            assert not s['command']['template_available']
            assert s['command']['frame_source'] == 'generated-touch-e3'
            # No capture/import/save/adopt between pairing and command.
            # Command without imported hex: requested first, old decoded report,
            # state transmission, short ACK, then a later matching decoded report.
            for target, mode, expected in ((2050, 0, COMFORT), (1550, 3, ECO),
                (700, 2, '38F1010203C6F10405064500004B02A83B004C00008E000084CC00000000972030049401BE02E100641919DA000000005B0078B2173B3B9450290A'),
                (0, 1, '38F1010203C6F10405064500004B02A83B004C00008E000084CC000000009720300494014001E100641919DA000000005B0078B2173B3B7DE8F50D')):
                app.command(target, mode)
                s = app.advance(2)
                assert s['command']['phase'] == 'waiting_report'
                assert s['command']['requested_target_c'] == target / 100
                assert s['simulation_tx'][-1] == POLL
                s = app.advance(301)
                assert s['command']['phase'] == 'waiting_readback'
                assert s['simulation_tx'][-1] == expected
                assert s['observed']['target_c'] != target / 100
                s = app.advance(100)  # ACK without report cannot match.
                assert s['command']['phase'] == 'waiting_readback'
                s = app.advance(200)
                assert s['command']['phase'] == 'matched' and s['command']['attempts'] == 1
                assert abs(s['observed']['target_c'] - target / 100) < 0.03
                assert s['observed']['mode'] == mode
                if mode == 0:
                    assert s['last_frame'] == '23F1040506C6F1010203DF203000404B02A88A02B100E100641919DA000A000A0A47C783C11D'
            # Valid wrong-address or corrupt reports cannot update paired observation.
            previous = s['observed'].copy()
            valid, invalid = s['valid_frames'], s['invalid_frames']
            for f in (changed_frame(REPORT, 4), changed_frame(REPORT, 9), REPORT[:-2] + '00'):
                app.sim('inject', frame=f)
                s = app.advance(1)
                assert s['observed']['target_c'] == previous['target_c']
                assert s['observed']['mode'] == previous['mode']
            assert s['valid_frames'] == valid + 2 and s['invalid_frames'] == invalid + 1
            # Silence expires at the unchanged production wait, without blind retry.
            app.sim('scenario', name='silence')
            app.command(2200)
            s = app.advance(2)
            assert s['command']['phase'] == 'waiting_report'
            assert app.advance(149999)['command']['phase'] == 'waiting_report'
            s = app.advance(1)
            assert s['command']['phase'] == 'timeout' and s['command']['attempts'] == 0
            tx = s['simulation_tx']
            assert app.advance(300000)['simulation_tx'] == tx
            # ACK-only after valid state TX never establishes application.
            app.sim('scenario', name='ack_only')
            app.command(2200)
            s = app.advance(1000)
            assert s['command']['phase'] == 'waiting_readback' and s['command']['attempts'] == 1
            s = app.advance(150000)
            assert s['command']['phase'] == 'timeout' and s['command']['attempts'] == 1
            # Fresh mismatching readback permits only one retry (two total).
            app.command(2200)
            app.advance(1000)
            app.sim('inject', frame=REPORT)
            s = app.advance(2)
            assert s['command']['attempts'] == 2
            app.sim('inject', frame=REPORT)
            s = app.advance(2)
            assert s['command']['attempts'] == 2
            s = app.advance(150000)
            assert s['command']['phase'] == 'timeout'
            # Delayed scenario progresses only after the queued RF response is due.
            app.sim('scenario', name='delayed')
            app.command(2300)
            assert app.advance(2000)['command']['phase'] == 'waiting_report'
            s = app.advance(1000)
            assert s['command']['phase'] == 'waiting_readback'
            assert app.advance(3000)['command']['phase'] == 'matched'
            # A deprecated override fails explicitly, then a normal command works.
            app.http('/api/command', {'mode': 0, 'centi_c': 2300, 'clock_seconds': 0, 'state': STATE[:-2] + '00'}, 400)
            app.command(2400)
            assert app.advance(2)['command']['phase'] == 'waiting_report'
        finally:
            app.stop()
        app = App(storage)
        try:
            s = app.advance(600000)
            assert s['pairing']['saved'] and not s['command']['template_available']
            assert s['command']['phase'] == 'idle' and s['simulation_tx'] == []
            assert 'observed' not in s
            app.command()  # Saved relationship, still no template after restart.
            assert app.advance(1000)['command']['phase'] == 'matched'
        finally:
            app.stop()
        # Interactive mode has no raw injection or clock advance controls.
        app = App(Path(directory) / 'other.bin', test_clock=False)
        try:
            app.http('/api/simulation', {'action': 'advance', 'ms': 1000}, 400)
            app.http('/api/simulation', {'action': 'inject', 'frame': REPORT}, 400)
            app.http('/api/pairing', {'action': 'start', 'source': 'F1010203', 'destination': 'F1100001'}, 202)
            # An accepted connection must not stall other clients or clock work.
            # Exercise idle sockets, incomplete headers and incomplete POST bodies.
            for partial in (b'', b'GET /api/radio HTTP/1.0\r\n',
                            b'POST /api/command HTTP/1.0\r\nContent-Length: 20\r\n'
                            b'Content-Type: application/json\r\n\r\n{'):
                before = app.http()['simulation_time_ms']
                port = int(app.url.rsplit(':', 1)[1])
                with socket.create_connection(('127.0.0.1', port), timeout=2) as idle:
                    if partial:
                        idle.sendall(partial)
                    time.sleep(0.05)  # Let the single-threaded server accept it.
                    started = time.monotonic()
                    with urllib.request.urlopen(app.url + '/api/radio', timeout=2) as response:
                        status = json.load(response)
                    assert time.monotonic() - started < 2
                    assert status['simulation_time_ms'] > before
                assert app.http()['ready']
            deadline = time.monotonic() + 5
            while not app.http()['pairing']['saved']:
                assert time.monotonic() < deadline, 'Interactive clock failed to complete pairing'
                time.sleep(0.05)
        finally:
            app.stop()
    print('HTTP simulation: generated no-template pairing/control, all four full-byte mode vectors, retry context/freshness/deadlines, preserved legacy diagnostics/storage/restarts, requested/readback, silence/ACK-only and invalid traffic: passed')


if __name__ == '__main__':
    main()
