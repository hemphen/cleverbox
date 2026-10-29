#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Production MQTT routing/publication, HTTP configuration, time and RF with fake client I/O."""
import json
import secrets
import tempfile
from pathlib import Path
from test_http import App, STATE, REPORT

BASE='cleverbox/v1/020000000001'


def publications(app):
    return app.http('/api/simulation/mqtt')


def connect(app):
    app.sim('mqtt_connection',connected=True); app.advance(100)
    s=app.http('/api/mqtt'); assert s['connected'] and len(s['session'])==32
    return s['session']


def send(app, session, seq, id=1, retained=False, payload=None, **fields):
    if payload is None: payload=json.dumps(dict(session=session,request_id=seq,mode=0,centi_c=2050,**fields))
    app.sim('mqtt_input',topic=f'{BASE}/radiators/{id}/set',payload=payload,retained=retained)
    app.advance(100)
    rows=publications(app)
    return [r['payload'] for r in rows if r['topic'].endswith('/result') and r['payload']['stage']=='admission'][-1]


def run(directory):
    storage=Path(directory)/'mqtt.bin'; storage.write_bytes(bytes.fromhex('01F1010203F1040506'))
    app=App(storage)
    secret=secrets.token_hex(16)
    config=dict(action='configure',host='broker.test',port=1883,username='bench',password=secret,enabled=True)
    try:
        s=app.http('/api/mqtt'); assert not s['configured'] and not s['enabled'] and not s['connected']
        for bad in ({},dict(action='enable'),dict(config,port=0),dict(config,host='mqtt://broker'),
                    dict(config,password='x\0y'),dict(config,username=''),dict(config,enabled=1),dict(config,extra=1)):
            app.http('/api/mqtt',bad,400)
        app.http('/api/mqtt',config,202); app.http('/api/mqtt',config,409); app.advance(100)
        s=app.http('/api/mqtt'); assert s['configured'] and s['enabled'] and not s['connected'] and not s['pending']
        assert secret not in json.dumps(s) and 'password' not in s
        app.http('/api/network',dict(action='configure',ssid=secrets.token_hex(6),password=secrets.token_hex(8)),202)
        app.sim('network_event',name='started'); app.sim('network_event',name='got_ip'); app.advance(100)
        session=connect(app); rows=publications(app)
        assert any(r['payload']=='online' and r['retain'] for r in rows)
        initial=[r['payload'] for r in rows if r['topic'].endswith('/state')][-1]
        assert initial['available'] is False and 'mode' not in initial and 'age_ms' not in initial
        assert app.http()['simulation_tx']==[]
        assert send(app,session,1)['reason']=='time_unavailable'
        app.http('/api/time',dict(action='manual',unix_seconds=1767268800))
        assert send(app,session,2)['reason']=='busy_or_unavailable_or_missing_template'
        app.http('/api/controller-state',dict(action='import',id=1,state=STATE),202); app.advance(1)
        assert send(app,session,3,id=999)['reason']=='unknown_identity'
        for seq,body in enumerate((dict(session=session,request_id=4,mode=0),
                          dict(session=session,request_id=5,centi_c=2050),
                          dict(session=session,request_id=6,mode=None,centi_c=2050),
                          dict(session=session,request_id=7,mode=0,centi_c=2050,clock_seconds=1)),4):
            assert send(app,session,seq,payload=json.dumps(body))['reason']=='invalid_fields'
        assert send(app,session,8,retained=True)['reason']=='retained_command'
        assert send(app,session,8)['accepted']
        assert send(app,session,8)['reason']=='duplicate_or_out_of_order'
        assert send(app,session,9)['reason']=='busy_or_unavailable_or_missing_template'
        app.advance(1000)
        results=[r['payload'] for r in publications(app) if r['topic'].endswith('/result')]
        assert any(r['stage']=='delivery' and r['phase']=='matched' and r['request_id']==8 for r in results)
        # Out-of-order rejections remain rejected even after a device becomes idle.
        assert send(app,session,9)['reason']=='duplicate_or_out_of_order'
        assert send(app,session,10)['accepted']; app.advance(1000); publications(app)
        # Complete flat JSON fragmented at every byte boundary; total=512 is allowed.
        payload=json.dumps(dict(session=session,request_id=11,mode=3,centi_c=1550))
        payload+=' '*(512-len(payload))
        for offset in range(0,len(payload),23):
            app.sim('mqtt_input',topic=f'{BASE}/radiators/1/set' if offset==0 else '',payload=payload[offset:offset+23],offset=offset,total=len(payload))
        app.advance(100); rows=publications(app)
        assert any(r['payload'].get('accepted') for r in rows)
        app.advance(1000); publications(app)
        tx=app.http()['simulation_tx']
        # Oversized, invalid fragments, NUL, deep JSON, wrong/malformed identities and session.
        for payload,offset,total in [(' '*513,0,513),('x',2,100),('x\0y',0,3),('{'*200,0,200)]:
            app.sim('mqtt_input',topic=f'{BASE}/radiators/1/set',payload=payload,offset=offset,total=total)
        app.advance(100)
        assert app.http()['simulation_tx']==tx and app.http('/api/mqtt')['dropped_input']>=3
        assert send(app,'old-session',12)['reason']=='session_mismatch'
        for identity in ('0','-1','01','4294967296','foo'):
            app.sim('mqtt_input',topic=f'{BASE}/radiators/{identity}/set',payload='{}')
            app.advance(100)
            assert any(r['topic'].endswith('/errors') for r in publications(app))
        # A command's requested settings cannot replace the received observation.
        app.sim('scenario',name='delayed')
        assert send(app,session,12,payload=json.dumps(dict(session=session,request_id=12,mode=0,centi_c=2500)))['accepted']
        assert app.http()['observed']['target_c']==15.5
        app.advance(1000); assert app.http()['observed']['target_c']==15.5
        # Broker loss after admission must not stop/restart RF. Reconnect has new nonce.
        app.sim('mqtt_connection',connected=False); app.advance(6000)
        assert app.http()['command']['phase']=='matched' and app.http()['observed']['target_c']==25
        tx=app.http()['simulation_tx']; new_session=connect(app); assert new_session!=session
        rows=publications(app)
        assert any(r['payload'].get('stage')=='delivery' and r['payload']['request_id']==12 and r['payload']['session']==session for r in rows if isinstance(r['payload'],dict))
        assert send(app,session,13)['reason']=='session_mismatch'
        assert app.http()['simulation_tx']==tx
        # Lost connection clears pending input before admission, including partial input.
        app.sim('mqtt_input',topic=f'{BASE}/radiators/1/set',payload=json.dumps(dict(session=new_session,request_id=1,mode=0,centi_c=2100)))
        app.sim('mqtt_connection',connected=False); app.advance(100); new_session=connect(app)
        assert app.http()['simulation_tx']==tx
        # Queue pressure is bounded; no input admitted while disconnected.
        dropped=app.http('/api/mqtt')['dropped_input']
        for _ in range(6):
            app.sim('mqtt_input',topic=f'{BASE}/radiators/1/set',payload='{}')
        app.advance(100)
        assert app.http('/api/mqtt')['dropped_input']==dropped+2
        # Partial assemblies with a gap cannot turn into a complete command.
        incomplete=json.dumps(dict(session=new_session,request_id=1,mode=0,centi_c=2100))
        app.sim('mqtt_input',topic=f'{BASE}/radiators/1/set',payload=incomplete[:20],total=len(incomplete))
        app.sim('mqtt_input',topic='',payload=incomplete[21:],offset=21,total=len(incomplete))
        app.advance(100); assert app.http()['simulation_tx']==tx
        # A tracked result survives a later HTTP admission before the MQTT tick.
        app.sim('scenario',name='normal')
        assert send(app,new_session,1)['accepted']
        for _ in range(1000):
            if app.http()['command']['phase']=='matched': break
            app.advance(1)
        assert app.http()['command']['phase']=='matched'
        app.http('/api/command',dict(id=1,mode=3,centi_c=1550),202)
        app.advance(100)
        rows=publications(app)
        assert any(isinstance(r['payload'],dict) and r['payload'].get('phase')=='matched' and r['payload']['request_id']==1 for r in rows)
        app.advance(1000)
        # Publication staleness grows from reception; restored templates never substitute.
        app.advance(300000); rows=publications(app)
        last=[r['payload'] for r in rows if r['topic'].endswith('/state')][-1]
        assert last['stale'] and last['age_ms']>=300000 and not any(r['retain'] for r in rows if r['topic'].endswith('/state'))
        # Include another radiator and prove independent completion / same-device busy.
        app.sim('scenario',name='normal'); app.sim('device',index=1)
        app.http('/api/pairing',dict(action='start',source='F1010203',destination='F1100001'),202); app.advance(2000)
        app.sim('fixture'); app.advance(1)
        app.http('/api/controller-state',dict(action='save',id=2),202); app.advance(1)
        app.sim('scenario',name='silence_a')
        assert send(app,new_session,2)['accepted']
        assert send(app,new_session,3,id=2)['accepted']
        assert send(app,new_session,4)['reason']=='busy_or_unavailable_or_missing_template'
        app.advance(1000)
        radiators=app.http()['radiators']
        assert radiators[0]['command']['pending'] and radiators[1]['command']['phase']=='matched'
        # Disabling only MQTT preserves active RF; graceful offline is attempted.
        app.http('/api/mqtt',dict(action='disable'),202); app.advance(100)
        rows=publications(app); assert any(r['payload']=='offline' for r in rows)
        assert not app.http('/api/mqtt')['enabled']
        app.advance(150000); assert app.http()['radiators'][0]['command']['phase']=='timeout'
        # Wi-Fi loss does not replay commands; enable/reconnect is state-only.
        app.http('/api/mqtt',dict(action='enable'),202); app.advance(100)
        app.sim('network_event',name='disconnected'); app.advance(100)
        assert not app.http('/api/mqtt')['connected']
        tx=app.http()['simulation_tx']; app.stop(); app=App(storage)
        app.advance(1000)
        assert app.http()['simulation_tx']==[] and all('observed' not in r for r in app.http()['radiators'])
        assert app.http('/api/mqtt')['enabled'] and app.http('/api/time')['usable'] is False
        assert all(r['command']['phase']=='idle' for r in app.http()['radiators'])
        # Actual restart/reconnect rejects old-session commands even with low IDs.
        app.sim('network_event',name='started'); app.sim('network_event',name='got_ip'); app.advance(100)
        restarted=connect(app); assert restarted not in (session,new_session)
        assert send(app,new_session,1)['reason']=='session_mismatch' and app.http()['simulation_tx']==[]
        # A failed durable save keeps this boot's RAM configuration, reports
        # uncertainty and does not claim that old settings will survive reboot.
        before=app.http('/api/mqtt')
        app.sim('mqtt_storage_fault',mode=1)
        app.http('/api/mqtt',dict(config,host='replacement.test'),202); app.advance(100)
        failed=app.http('/api/mqtt')
        assert failed['persistence_uncertain'] and failed['config_error'] and failed['host']==before['host']
        assert failed['connected'] and failed['session']==before['session']
        app.http('/api/mqtt',config,202); app.advance(100)
        assert not app.http('/api/mqtt')['persistence_uncertain']
        # Failed forget is also honest; retry force-writes a verified tombstone.
        app.sim('mqtt_storage_fault',mode=2)
        app.http('/api/mqtt',dict(action='forget'),202); app.advance(100)
        assert app.http('/api/mqtt')['persistence_uncertain'] and app.http('/api/mqtt')['configured']
        app.http('/api/mqtt',dict(action='forget'),202); app.advance(100)
        s=app.http('/api/mqtt'); assert not s['configured'] and not s['enabled'] and s['username']==''
        assert app.http('/api/network')['configured'] and len(app.http()['radiators'])==2
        assert secret not in json.dumps(publications(app))
    finally: app.stop()


if __name__=='__main__':
    with tempfile.TemporaryDirectory() as directory: run(directory)
    print('MQTT fake-client integration: configuration/privacy, admission/results/observations, duplicates/session/retained/fragments, independent RF, reconnect and restart: passed')
