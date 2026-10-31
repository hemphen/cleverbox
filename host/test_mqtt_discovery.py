#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Production discovery with fake socket I/O; actual HA is a separate suite."""
import json
import os
import tempfile
from pathlib import Path
from test_http import App, REPORT, changed_frame
from test_mqtt import connect, publications
from test_registry import registry_blob


def run(directory):
    storage=Path(directory)/'discovery.bin'
    storage.write_bytes(bytes.fromhex('01F1010203F1040506'))
    app=App(storage)
    def restart(fail=0):
        nonlocal app
        app.stop(); app=App(storage)
        app.sim('network_event',name='started'); app.sim('network_event',name='got_ip'); app.advance(100)
        app.sim('mqtt_confirmed_mode',mode=fail)
        if fail==1:
            app.sim('mqtt_connection',connected=True); app.advance(100)
            mid=app.http('/api/simulation/mqtt-client')['subscription_id']
            app.sim('mqtt_suback',id=mid,qos=0); app.advance(100)
        else: connect(app)
    def configs(ms=7000):
        app.advance(ms)
        return [r for r in publications(app) if r['topic'].startswith('homeassistant/')]
    try:
        assert not app.http('/api/mqtt')['discovery_enabled']
        app.http('/api/mqtt',dict(action='configure',host='broker.test',port=1883,username='',password='',enabled=True),202)
        app.http('/api/network',dict(action='configure',ssid='synthetic',password='synthetic'),202)
        app.sim('network_event',name='started'); app.sim('network_event',name='got_ip'); app.advance(100); connect(app)
        assert not configs()
        name='"\\'*16
        app.http('/api/radiators',dict(action='rename',id=1,name=name),202); app.advance(100)
        # Storage failure can occur after durable mutation; halt until explicit retry.
        app.sim('discovery_storage_fault',mode=1)
        app.http('/api/mqtt',dict(action='discovery_enable'),202); app.advance(100)
        assert app.http('/api/mqtt')['discovery_error'] and not app.http('/api/mqtt')['discovery_enabled']
        assert not configs()
        app.http('/api/mqtt',dict(action='discovery_enable'),202)
        rows=configs(); assert rows
        c=rows[-1]['payload']; assert c['device']['name']==name and len(json.dumps(c).encode())<8192
        assert len(c['components'])==7 and all(x['expire_after']==15 for x in c['components'].values())
        assert len({x['unique_id'] for x in c['components'].values()})==7
        assert all(x['unique_id'].startswith('cleverbox_020000000001_1_') for x in c['components'].values())
        assert all(r['retain'] for r in rows) and app.http()['simulation_tx']==[]
        app.advance(100); journal_before=Path(str(storage)+'.ha_discovery').read_bytes()
        assert not configs(15000)
        assert Path(str(storage)+'.ha_discovery').read_bytes()==journal_before
        app.http('/api/mqtt',dict(action='configure',host='other.test',port=1883,username='',password='',enabled=True),409)
        app.http('/api/mqtt',dict(action='forget'),409)
        app.sim('mqtt_input',topic='homeassistant/status',payload='online'); app.advance(100)
        assert configs(1000)[-1]['topic']==rows[-1]['topic']
        # Immediate publication failure: keep durable ownership, pace retries.
        app.sim('mqtt_confirmed_mode',mode=1); app.sim('mqtt_input',topic='homeassistant/status',payload='online'); configs()
        assert not configs(1000)
        assert app.http('/api/mqtt')['discovery_error']==0x7413 and not app.http('/api/mqtt')['connected']
        app.sim('mqtt_confirmed_mode',mode=0); app.advance(5000); connect(app); assert configs()
        # Missing PUBACK discards the stuck outbox with a paced client restart.
        app.sim('mqtt_confirmed_mode',mode=2); app.sim('mqtt_input',topic='homeassistant/status',payload='online'); configs(12000)
        assert not app.http('/api/mqtt')['connected']
        app.sim('mqtt_confirmed_mode',mode=0); app.advance(5000); connect(app); assert configs()
        app.advance(5000); state=[r['payload'] for r in publications(app) if r['topic'].endswith('/state')][-1]
        assert not state['available'] and not state['fresh'] and 'temperature_centi_c' not in state
        app.sim('inject',frame=REPORT); app.advance(5000)
        state=[r['payload'] for r in publications(app) if r['topic'].endswith('/state')][-1]
        assert state['fresh'] and state['temperature_centi_c']==2000
        missing=bytearray.fromhex(REPORT); missing[16:18]=bytes.fromhex('084C'); missing[16]^=1
        app.sim('inject',frame=changed_frame(missing.hex(),16)); app.advance(5000)
        state=[r['payload'] for r in publications(app) if r['topic'].endswith('/state')][-1]
        assert state['fresh'] and 'temperature_centi_c' not in state and 'target_centi_c' in state
        app.advance(286000)
        state=[r['payload'] for r in publications(app) if r['topic'].endswith('/state')][-1]
        assert not state['fresh']
        # Offline removal survives process restart and failed cleanup publication.
        app.sim('mqtt_connection',connected=False)
        app.http('/api/radiators',dict(action='remove',id=1),202); app.advance(100)
        restart(fail=1); publications(app); app.advance(6000)
        assert app.http('/api/mqtt')['discovery_pending']>=1
        restart(); rows=configs(); assert any(all(set(c)=={'platform'} for c in r['payload']['components'].values()) and r['retain'] for r in rows)
        app.advance(100); assert app.http('/api/mqtt')['discovery_pending']==0
        assert app.http()['simulation_tx']==[]
        app.http('/api/mqtt',dict(action='discovery_disable'),202); app.advance(100)
        restart(); assert not app.http('/api/mqtt')['discovery_enabled']
        # Full journal backpressure: no untracked publish until old cleanup ACKs.
        capacity=app.http()['capacity']; app.stop()
        journal=Path(str(storage)+'.ha_discovery')
        journal.write_bytes(bytes([2,1,0,0])+bytes.fromhex('01000000')+
                            b''.join(i.to_bytes(8,'little') for i in range(100,100+4*capacity)))
        Path(str(storage)+'.registry').write_bytes(registry_blob(1))
        app=App(storage)
        app.sim('network_event',name='started'); app.sim('network_event',name='got_ip'); app.advance(100)
        app.sim('mqtt_confirmed_mode',mode=2); connect(app)
        rows=configs(2000); assert rows and all(all(set(c)=={'platform'} for c in r['payload']['components'].values()) for r in rows)
        assert app.http('/api/mqtt')['discovery_pending']>=4*capacity
        restart(); rows=configs(20000)
        assert any(r['topic'].endswith('_1/config') and 'unique_id' in r['payload']['components']['mode'] for r in rows)
        assert app.http()['simulation_tx']==[]
        app.stop(); journal.write_bytes(b'broken'); app=App(storage)
        assert app.http('/api/mqtt')['discovery_error']
        app.http('/api/mqtt',dict(action='discovery_enable'),202); app.advance(100)
        assert journal.read_bytes()==b'broken'
    finally: app.stop()


def disconnect_outbox(directory, fast_reconnect=False):
    storage=Path(directory)/('fast-outbox.bin' if fast_reconnect else 'outbox.bin')
    storage.write_bytes(bytes.fromhex('01F1010203F1040506'))
    app=App(storage)
    try:
        app.http('/api/mqtt',dict(action='configure',host='broker.test',port=1883,username='',password='',enabled=True),202)
        app.http('/api/network',dict(action='configure',ssid='synthetic',password='synthetic'),202)
        app.sim('network_event',name='started'); app.sim('network_event',name='got_ip'); app.advance(100); connect(app)
        app.sim('mqtt_confirmed_mode',mode=2)
        app.http('/api/mqtt',dict(action='discovery_enable'),202); app.advance(500)
        rows=[r for r in publications(app) if r['topic'].startswith('homeassistant/')]
        assert len(rows)==1 and 'unique_id' in rows[0]['payload']['components']['mode']
        assert app.http('/api/simulation/mqtt-client')['outbox_pending']==1
        starts=app.http('/api/simulation/mqtt-client')['starts']
        app.sim('mqtt_connection',connected=False)
        if fast_reconnect:
            # Callback coalescing cannot hide a disconnect from the owner.
            app.sim('mqtt_connection',connected=True)
        app.http('/api/radiators',dict(action='remove',id=1),202); app.advance(100)
        app.sim('mqtt_confirmed_mode',mode=0)
        # Allow a surviving old client to reconnect before the owner retries.
        if not app.http('/api/simulation/mqtt-client')['outbox_pending']:
            assert not app.http('/api/mqtt')['connected']
            app.advance(4900)
            assert app.http('/api/simulation/mqtt-client')['starts']==starts
            app.advance(200)
        connect(app); app.advance(1000)
        rows=[r for r in publications(app) if r['topic'].startswith('homeassistant/')]
        assert rows and all(all(set(c)=={'platform'} for c in r['payload']['components'].values()) for r in rows)
        app.advance(100); assert app.http('/api/mqtt')['discovery_pending']==0
        # Try the stale retransmission AFTER successful removal. Old code retains
        # and delivers this active config; client destruction must make it inert.
        app.sim('mqtt_replay_outbox'); app.advance(100)
        assert not any(r['topic'].startswith('homeassistant/') for r in publications(app))
        state=app.http('/api/simulation/mqtt-client')
        assert state['replay_attempts']==1 and state['replayed']==0
        assert state['outbox_pending']==0 and state['outbox_discarded']==1
        assert state['starts']==starts+1
        assert app.http()['simulation_tx']==[]
    finally: app.stop()


def payload_limit(directory):
    path=Path(directory)/'limit.bin'; path.write_bytes(bytes.fromhex('01F1010203F1040506'))
    app=App(path)
    try:
        app.http('/api/mqtt',dict(action='configure',host='broker.test',port=1883,username='',password='',enabled=True),202)
        app.advance(100); app.http('/api/mqtt',dict(action='discovery_enable'),202); app.advance(100)
        app.http('/api/network',dict(action='configure',ssid='synthetic',password='synthetic'),202)
        app.sim('network_event',name='started'); app.sim('network_event',name='got_ip'); app.advance(100); connect(app)
        app.advance(6000)
        assert app.http('/api/mqtt')['discovery_error']==0x7412 and app.http('/api/mqtt')['connected']
        assert not any(r['topic'].startswith('homeassistant/') for r in publications(app))
        assert app.http()['simulation_tx']==[]
    finally: app.stop()


if __name__=='__main__':
    with tempfile.TemporaryDirectory() as directory:
        if os.environ.get('CLEVERBOX_DISCOVERY_LIMIT_TEST'):
            payload_limit(directory)
        else:
            run(directory)
            disconnect_outbox(directory)
            disconnect_outbox(directory,fast_reconnect=True)
    print('Discovery boundary: bounded payload, stable IDs, birth, publish failure/PUBACK timeout, restart/offline cleanup, freshness, full journal and corruption: passed')
