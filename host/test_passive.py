#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Passive commissioning against production HTTP/worker/codec/NVS, fake RF only."""
from pathlib import Path
import tempfile
from test_http import App, STATE, REPORT
from test_http_boundary import request, files
from test_mqtt import connect, send, publications


def seal(f, repair_inner=True):
    f = bytearray(f)
    def crc(data, reflected):
        c = 0xffff
        for b in data:
            c ^= b if reflected else b << 8
            for _ in range(8):
                c = ((c >> 1) ^ (0xa001 if c & 1 else 0)) if reflected else (((c << 1) ^ (0x8005 if c & 0x8000 else 0)) & 0xffff)
        return c
    l = f[0]
    if repair_inner:
        f[l-1:l+1] = crc(f[1:l-1], True).to_bytes(2, 'little')
    f[l+1:l+3] = crc(f[:l+1], False).to_bytes(2, 'big')
    return f.hex().upper()


def changed(frame, offset, data):
    f = bytearray.fromhex(frame)
    f[offset:offset+len(data)] = data
    return seal(f)


def status(app): return app.http('/api/commissioning')
def inject(app, frame): app.sim('inject', frame=frame); app.advance(1)
def mode(app, value): app.http('/api/commissioning', {'mode': value}, 202); app.advance(1)
def no_tx(app): assert app.http('/api/simulation/radio')['simulation_tx'] == []
def adopt(app, candidate, id=1, replace=False, code=202):
    return app.http('/api/controller-state', dict(action='adopt', id=id, radiator=candidate['radiator'], version=candidate['version'], replace=replace), code)


def defaults_and_storage(directory):
    storage = directory / 'fresh'
    app = App(storage, operating_mode=None)
    try:
        s = status(app)
        assert s['mode'] == 'listen-only' and s['save_status'] == 'succeeded' and not s['storage_error']
        assert Path(str(storage)+'.rf_mode').read_bytes() == b'\1\0'
        # Setup creates other keys; future boots must stay passive.
        app.http('/api/time', dict(action='timezone', timezone='UTC0'), 200)
        app.http('/api/pairing', dict(action='start', source='F1010203', destination='F1100001'), 409)
        app.http('/api/command', dict(mode=0, centi_c=2050, clock_seconds=0), 409)
        no_tx(app)
    finally: app.stop()
    app = App(storage, operating_mode=None)
    try:
        assert status(app)['mode'] == 'listen-only'
        mode(app, 'control-enabled'); no_tx(app)
    finally: app.stop()
    app = App(storage, operating_mode=None)
    try:
        assert status(app)['mode'] == 'control-enabled'; no_tx(app)
        # Both a pre-write refusal and an eager durable-but-failed write fail closed.
        for fault in (2, 1, 3):
            app.sim('mode_storage_fault', mode=fault)
            app.http('/api/commissioning', {'mode': 'listen-only'}, 202)
            queued = status(app); assert queued['pending'] and queued['save_status'] == 'pending'
            app.advance(1); s = status(app)
            assert s['mode'] == 'listen-only' and s['storage_error'] and s['persistence_uncertain'] and s['save_status'] == 'failed'
            app.http('/api/pairing', dict(action='start', source='F1010203', destination='F1100001'), 409)
            no_tx(app)
            app.sim('mode_storage_fault', mode=0); mode(app, 'control-enabled')
            assert not status(app)['storage_error']
        mode(app, 'listen-only')
    finally: app.stop()
    # Existing installations, including a deliberately empty registry, retain control.
    for suffix, data in (('',bytes.fromhex('01F1010203F1040506')), ('.registry',bytes.fromhex('01000001000000')), ('.wifi',b'corrupt')):
        path = directory / ('migrate'+(suffix or 'pair'))
        Path(str(path)+suffix).write_bytes(data)
        app = App(path, operating_mode=None)
        try: assert status(app)['mode'] == 'control-enabled'; no_tx(app)
        finally: app.stop()
    # Malformed and unreadable mode is never silently replaced, even on an upgrade.
    for data in (b'', b'\1', b'\2\1', b'\1\2', b'\1\0extra'):
        record = Path(str(storage)+'.rf_mode'); record.write_bytes(data)
        app = App(storage, operating_mode=None)
        try:
            assert status(app)['mode'] == 'listen-only' and status(app)['storage_error']
            assert record.read_bytes() == data; no_tx(app)
        finally: app.stop()
    record.unlink(); record.mkdir()
    app = App(storage, operating_mode=None)
    try: assert status(app)['storage_error'] and record.is_dir(); no_tx(app)
    finally: app.stop()


def initial_failure_wifi_reboot(directory):
    # No boot mode record, no settings, initial rf_mode write refused before mutation.
    storage = directory / 'initial-mode-failure'
    app = App(storage, operating_mode=None, mode_storage_fault=2, interfaces=True)
    try:
        s = status(app)
        assert s['mode'] == 'listen-only' and s['storage_error'] and not Path(str(storage)+'.rf_mode').exists()
        # All settings writers must stop before touching their own keys while
        # the initial decision cannot be stored. Read-only AP recovery survives.
        app.http('/api/time', dict(action='timezone',timezone='UTC0'),409)
        app.http('/api/mqtt',dict(action='configure',host='broker.test',port=1883,username='',password='',enabled=True),202)
        app.advance(100); assert app.http('/api/mqtt')['config_error']
        app.http('/api/mqtt',dict(action='discovery_enable'),202)
        app.advance(100); assert app.http('/api/mqtt')['discovery_error']
        request(app, '127.0.0.2', '/api/network', dict(action='configure',ssid='synthetic',password='testonly123'),202)
        app.sim('network_event',name='started'); app.sim('network_event',name='got_ip'); app.advance(100)
        assert app.http('/api/network')['setup_result'] == 'failed'
        for suffix in ('.rf_mode','.wifi','.timezone','.mqtt','.ha_discovery','.registry'):
            assert not Path(str(storage)+suffix).exists(), suffix
        assert request(app,'127.0.0.2','/api/commissioning')['mode'] == 'listen-only'
        # A later Wi-Fi write succeeds once storage recovers, using AP setup.
        app.sim('mode_storage_fault', mode=0)
        request(app, '127.0.0.2', '/api/network', dict(action='configure',ssid='synthetic',password='testonly123'),202)
        app.sim('network_event',name='started'); app.sim('network_event',name='got_ip'); app.advance(100)
        assert app.http('/api/network')['setup_result'] == 'saved'
        assert Path(str(storage)+'.wifi').exists()
        assert Path(str(storage)+'.rf_mode').read_bytes() == b'\1\0'
        assert status(app)['mode'] == 'listen-only' and status(app)['storage_error']
        no_tx(app)
    finally: app.stop()
    app = App(storage, operating_mode=None)
    try:
        assert status(app)['mode'] == 'listen-only', 'Wi-Fi save must not turn first-boot failure into legacy control migration'
        assert Path(str(storage)+'.rf_mode').read_bytes() == b'\1\0'
        no_tx(app)
    finally: app.stop()


def corrupt_mode_recovery(directory):
    storage = directory / 'corrupt-mode-recovery'
    record = Path(str(storage)+'.rf_mode'); record.write_bytes(b'corrupt')
    app = App(storage, operating_mode=None, interfaces=True)
    try:
        assert status(app)['mode'] == 'listen-only' and status(app)['storage_error']
        request(app, '127.0.0.2', '/api/network', dict(action='configure',ssid='synthetic',password='testonly123'),202)
        app.sim('network_event',name='started'); app.sim('network_event',name='got_ip'); app.advance(100)
        assert app.http('/api/network')['setup_result'] == 'saved'
        assert record.read_bytes() == b'corrupt'
        no_tx(app)
    finally: app.stop()
    app = App(storage, operating_mode=None)
    try:
        assert app.http('/api/network')['configured']
        assert status(app)['mode'] == 'listen-only' and status(app)['storage_error']
        assert record.read_bytes() == b'corrupt'; no_tx(app)
    finally: app.stop()


def inventory_and_workflow(directory):
    storage = directory / 'workflow'
    app = App(storage, operating_mode=None, interfaces=True)
    # Invented controller address differs from Cleverbox's future pairing source.
    captured = changed(STATE, 1, bytes.fromhex('F10A0B0C'))
    heard = changed(REPORT, 6, bytes.fromhex('F10A0B0C'))
    try:
        baseline = files(storage)
        inject(app, captured)
        original = status(app)['candidates'][0]
        assert original['radiator'] == 'F1040506' and original['controller'] == 'F10A0B0C' and not original['paired']
        inject(app, heard)
        inject(app, changed(heard, 4, b'\7'))
        inject(app, '0FF1040506C6F101020301000A47E6E64D5A')
        bad = bytearray.fromhex(captured); bad[-1] ^= 1; inject(app, bad.hex())
        # Inner CRC broken while outer checksum is valid.
        bad = bytearray.fromhex(captured); bad[-3] ^= 1
        inject(app, seal(bad, repair_inner=False))
        inject(app, changed(captured, 10, b'\x44')) # CRC valid, wrong structure
        s = status(app)
        assert len(s['observations']) == 2 and len(s['candidates']) == 1 and s['candidates'][0]['version'] == original['version']
        assert not app.http()['radiators'] and files(storage) == baseline
        # Unavailable values and monotonic age/staleness, independent of UTC.
        missing = changed(heard, 16, b'\x08\x4c'); missing = changed(missing, 19, b'\x08\x4c')
        inject(app, missing)
        o = next(o for o in status(app)['observations'] if o['radiator'] == 'F1040506')
        assert o['temperature_c'] is None and o['target_c'] is None
        app.advance(300000)
        assert all(o['stale'] for o in status(app)['observations'])
        no_tx(app)
        # GET exposure chosen explicitly. All new mutations denied on AP/unknown.
        for address in ('127.0.0.2', '127.0.0.3'):
            request(app, address, '/api/commissioning')
            request(app, address, '/api/commissioning', {'mode':'control-enabled'}, 403)
            request(app, address, '/api/controller-state', dict(action='adopt',id=1,radiator=original['radiator'],version=original['version'],replace=True), 403)
        assert files(storage) == baseline; no_tx(app)
        # Candidate retained before pairing. Enabling itself creates zero work.
        mode(app, 'control-enabled'); no_tx(app)
        assert not app.http()['active_commands'] and not app.http()['exchange_id']
        app.http('/api/pairing', dict(action='start',source='F1010203',destination='F1100001'), 202)
        assert 'busy' in app.http('/api/commissioning', {'mode':'listen-only'},409).lower()
        app.advance(2500)
        radio = app.http(); assert len(radio['simulation_tx']) == 2 and radio['pairing']['saved']
        assert not radio['command']['template_available']
        candidate = status(app)['candidates'][0]
        assert candidate['paired_id'] == 1 and not candidate['saved_template']
        # Passive mode permits explicit adoption; queuing does not promise success.
        mode(app,'listen-only')
        adopt(app,candidate); assert status(app)['adoption']['status'] == 'pending'
        app.advance(1); assert status(app)['adoption']['status'] == 'succeeded'
        tx = app.http()['simulation_tx']; assert len(tx) == 2
        assert app.http()['command']['template_source'] == 'saved'
        adopt(app,candidate,code=409)
        # Persistence failure leaves the old saved bytes in service.
        registry = Path(str(storage)+'.registry'); previous = registry.read_bytes()
        registry.unlink(); registry.mkdir(); (registry/'block').write_bytes(b'x')
        adopt(app,candidate,replace=True); app.advance(1)
        assert status(app)['adoption']['status'] == 'failed' and status(app)['adoption']['error']
        (registry/'block').unlink(); registry.rmdir(); registry.write_bytes(previous)
        adopt(app,candidate,replace=True); app.advance(1)
        assert status(app)['adoption']['status'] == 'succeeded'
        # MQTT shares admission. A valid session/token cannot enable RF.
        app.http('/api/network',dict(action='configure',ssid='synthetic',password='testonly123'),202)
        app.sim('network_event',name='started'); app.sim('network_event',name='got_ip'); app.advance(100)
        app.http('/api/mqtt',dict(action='configure',host='broker.test',port=1883,username='',password='',enabled=True),202); app.advance(100)
        session = connect(app)
        result = send(app,session,1)
        assert not result['accepted'] and result['reason'] == 'listen_only'
        assert app.http()['simulation_tx'] == tx
        app.sim('mqtt_connection',connected=False); app.advance(5100); session = connect(app)
        assert status(app)['mode'] == 'listen-only'
        assert send(app,session,1)['reason'] == 'listen_only'
        assert app.http()['simulation_tx'] == tx
        mode(app,'control-enabled'); assert app.http()['simulation_tx'] == tx
        app.command(); app.advance(1000)
        assert app.http()['command']['phase'] == 'matched'
        sent = app.http()['simulation_tx']; assert len(sent) == 4 # offer, bind, poll, state only
        frame = bytes.fromhex(sent[-1])
        assert frame[1:5] == bytes.fromhex('F1010203') and frame[6:10] == bytes.fromhex('F1040506')
        assert frame[32] == bytes.fromhex(captured)[32] and frame[49:51] == bytes.fromhex(captured)[49:51]
        assert frame[31] == 0x20 and frame[33] & 1 == 0 and frame[39:43] == bytes.fromhex(REPORT)[23:27]
        # Version replacement and eviction, no cache indices in external identity.
        inject(app,captured)
        adopt(app,candidate,replace=True,code=409)
        cap = status(app)['capacity']
        for i in range(cap+1):
            inject(app,changed(captured,6,bytes([0xF1,0x20,i>>8,i&255])))
            inject(app,changed(heard,1,bytes([0xF1,0x21,i>>8,i&255])))
        s = status(app)
        assert len(s['observations']) == cap and len(s['candidates']) == cap
        assert s['observations_full'] and s['candidates_full'] and s['observation_evictions'] and s['candidate_evictions']
        adopt(app,candidate,replace=True,code=409)
        assert app.http()['simulation_tx'] == sent
        old_version = s['candidates'][-1]['version']
        mode(app,'listen-only')
    finally: app.stop()
    # RAM clears. Pair/template/mode persist. Old-boot selections cannot alias.
    app = App(storage, operating_mode=None)
    try:
        s = status(app)
        assert s['mode'] == 'listen-only' and not s['observations'] and not s['candidates']
        assert app.http()['command']['saved_available'] and app.http()['pairing']['saved']
        inject(app,captured)
        assert status(app)['candidates'][0]['version'] not in (old_version,original['version'])
        adopt(app,original,replace=True,code=409)
        app.advance(6000); no_tx(app)
    finally: app.stop()


if __name__ == '__main__':
    with tempfile.TemporaryDirectory() as directory:
        initial_failure_wifi_reboot(Path(directory))
        corrupt_mode_recovery(Path(directory))
        defaults_and_storage(Path(directory))
        inventory_and_workflow(Path(directory))
    print('Passive commissioning: defaults/migration/storage, reboot, HTTP/MQTT/AP guards, receive/caches, candidate adoption and exact end-to-end transmissions: passed')
