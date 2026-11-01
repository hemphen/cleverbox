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
        assert status(app)['mode'] == 'listen-only', 'Wi-Fi save must not turn first-boot failure into implicit control'
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
    storage=directory/'workflow'; app=App(storage,operating_mode=None,interfaces=True)
    heard=changed(REPORT,6,bytes.fromhex('F10A0B0C'))
    try:
        before=files(storage)
        inject(app,heard); inject(app,changed(heard,4,b'\7'))
        inject(app,STATE) # Valid non-report traffic has no retained capture path.
        s=status(app)
        assert len(s['observations'])==2 and 'candidates' not in s and 'adoption' not in s
        assert not app.http()['radiators'] and files(storage)==before
        cap=s['capacity']
        for i in range(cap+1): inject(app,changed(heard,1,bytes([0xF1,0x21,i>>8,i&255])))
        assert len(status(app)['observations'])==cap and status(app)['observation_evictions']
        app.advance(300000); assert all(o['stale'] for o in status(app)['observations'])
        no_tx(app)
        mode(app,'control-enabled'); no_tx(app)
        app.http('/api/pairing',dict(action='start',source='F1010203',destination='F1100001'),202)
        assert app.advance(2500)['pairing']['saved']
        mode(app,'listen-only'); tx=app.http()['simulation_tx']
        app.http('/api/command',dict(mode=0,centi_c=2050,clock_seconds=0),409)
        assert app.advance(1000)['simulation_tx']==tx
    finally: app.stop()
    app=App(storage,operating_mode=None)
    try:
        assert status(app)['mode']=='listen-only' and not status(app)['observations']
        assert app.http()['pairing']['saved']; app.advance(1000); no_tx(app)
    finally: app.stop()


if __name__ == '__main__':
    with tempfile.TemporaryDirectory() as directory:
        initial_failure_wifi_reboot(Path(directory))
        defaults_and_storage(Path(directory))
        inventory_and_workflow(Path(directory))
    print('Passive mode: defaults/storage failures/reboot, bounded inventory, explicit pairing and zero TX: passed')
