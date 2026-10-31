#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Selected passive HA: production receive/store/discovery, fake RF/socket I/O."""
import tempfile
import os
from pathlib import Path
from test_http import App, REPORT
from test_passive import changed, inject, status, no_tx, mode
from test_http_boundary import request
from test_mqtt import connect, publications, BASE

ADDRESS='F1040506'
TOPIC='homeassistant/device/cleverbox_020000000001_passive_'+ADDRESS+'/config'
STATE_TOPIC=BASE+'/passive/'+ADDRESS+'/state'


def select(app, address=ADDRESS, name='Passive room', action='select', code=202):
    body=dict(action=action,radiator=address)
    if action!='remove': body['name']=name
    app.http('/api/commissioning',body,code)
    if code==202: app.advance(1)


def setup(app):
    app.http('/api/mqtt',dict(action='configure',host='broker.test',port=1883,username='',password='',enabled=True),202)
    app.http('/api/network',dict(action='configure',ssid='synthetic',password='synthetic'),202)
    app.sim('network_event',name='started'); app.sim('network_event',name='got_ip'); app.advance(100)
    connect(app)


def enable(app, value=True):
    app.http('/api/mqtt',dict(action='discovery_enable' if value else 'discovery_disable'),202)
    app.advance(100)


def rows(app, ms=7100):
    app.advance(ms)
    return publications(app)


def snapshot(app):
    return [r['payload'] for r in rows(app) if r['topic']==STATE_TOPIC][-1]


def configs(app, ms=7100):
    return [r for r in rows(app,ms) if r['topic']==TOPIC]


def run(directory):
    storage=directory/'monitor'
    app=App(storage,operating_mode=None,interfaces=True)
    try:
        select(app,code=409) # Must have been observed; no arbitrary identities.
        heard=changed(REPORT,6,bytes.fromhex('F10A0B0C')); inject(app,heard)
        for host in ('127.0.0.2','127.0.0.3'):
            request(app,host,'/api/commissioning',dict(action='select',radiator=ADDRESS,name='Blocked'),403)
        for bad in (dict(action='select',radiator=ADDRESS,name=''),dict(action='select',radiator=ADDRESS,name='x'*33),
                    dict(action='rename',radiator=ADDRESS,name='a\0b'),dict(action='select',radiator=ADDRESS,name='a',extra=True),
                    dict(action='select',radiator='0',name='a'),dict(action='select',radiator=ADDRESS,name='a',mode='listen-only')):
            app.http('/api/commissioning',bad,400)
        select(app)
        s=status(app); assert s['monitor_save_status']=='succeeded' and len(s['monitors'])==1
        setup(app); assert not configs(app) and not any('/passive/' in r['topic'] for r in rows(app))
        enable(app); cfg=configs(app)[-1]['payload']
        assert len(cfg['components'])==5 and all(x['expire_after']==15 for x in cfg['components'].values())
        assert all(x['platform'] in ('sensor','binary_sensor') and 'command_topic' not in x for x in cfg['components'].values())
        assert cfg['state_topic']==STATE_TOPIC and cfg['device']['identifiers'].endswith('passive_'+ADDRESS)
        ids={c['unique_id'] for c in cfg['components'].values()}
        v=snapshot(app); assert v['controller']=='F10A0B0C' and v['fresh'] and v['temperature_centi_c']==2000
        assert not {'command_token','requested_centi_c','admission','delivery'} & v.keys()
        select(app,name='Renamed',action='rename'); cfg=configs(app)[-1]['payload']
        assert cfg['device']['name']=='Renamed' and {c['unique_id'] for c in cfg['components'].values()}==ids
        durable=Path(str(storage)+'.ha_monitors').read_bytes(); journal=Path(str(storage)+'.ha_discovery').read_bytes()
        # Evict the general cache. Dedicated selected storage is updated directly
        # on receive, not by scanning an evicting cache at publication time.
        cap=status(app)['capacity']
        for i in range(cap+5): inject(app,changed(heard,1,bytes([0xF1,0x20,i>>8,i&255])))
        assert ADDRESS not in [o['radiator'] for o in status(app)['observations']]
        heard=changed(heard,6,bytes.fromhex('D0123456')); inject(app,heard)
        for i in range(cap+5): inject(app,changed(heard,1,bytes([0xF1,0x21,i>>8,i&255])))
        assert snapshot(app)['controller']=='D0123456'
        # Invalid outer/inner CRC and unavailable markers do not fabricate readings.
        bad=bytearray.fromhex(heard); bad[-1]^=1; inject(app,bad.hex())
        assert snapshot(app)['controller']=='D0123456'
        missing=changed(changed(heard,16,b'\x08\x4c'),19,b'\x08\x4c'); inject(app,missing)
        v=snapshot(app); assert v['fresh'] and 'temperature_centi_c' not in v and 'target_centi_c' not in v
        app.advance(300000); assert not snapshot(app)['fresh']
        inject(app,heard); assert snapshot(app)['fresh']
        assert Path(str(storage)+'.ha_monitors').read_bytes()==durable
        assert Path(str(storage)+'.ha_discovery').read_bytes()==journal
        # Fill independent selections with observed addresses, report capacity honestly.
        cap=status(app)['monitor_capacity']
        for i in range(cap-1):
            a=f'F130{i:04X}'; inject(app,changed(heard,1,bytes.fromhex(a))); select(app,a)
        a='F1400001'; inject(app,changed(heard,1,bytes.fromhex(a))); select(app,a,code=409)
        no_tx(app)
    finally: app.stop()
    app=App(storage,operating_mode=None)
    try:
        assert status(app)['mode']=='listen-only' and len(status(app)['monitors'])==cap
        assert not any(m['available'] for m in status(app)['monitors'])
        app.sim('network_event',name='started'); app.sim('network_event',name='got_ip'); app.advance(100); connect(app)
        v=snapshot(app); assert not v['available'] and not v['fresh'] and 'temperature_centi_c' not in v
        inject(app,heard); assert snapshot(app)['fresh']
        enable(app,False); c=configs(app,20000)
        assert c and all(set(x)=={'platform'} for x in c[-1]['payload']['components'].values())
        assert not any(r['topic']==STATE_TOPIC for r in rows(app))
        assert len(status(app)['monitors'])==cap
        enable(app); c=configs(app,20000)[-1]['payload']
        assert {x['unique_id'] for x in c['components'].values()}==ids
        # Offline remove and reboot: cleanup journal retains original full address.
        app.sim('mqtt_connection',connected=False); select(app,action='remove')
        no_tx(app)
    finally: app.stop()
    app=App(storage,operating_mode=None)
    try:
        assert ADDRESS not in [m['radiator'] for m in status(app)['monitors']]
        app.sim('network_event',name='started'); app.sim('network_event',name='got_ip'); app.advance(100); connect(app)
        c=configs(app,20000); assert c and all(set(x)=={'platform'} for x in c[-1]['payload']['components'].values())
        assert app.http('/api/mqtt')['discovery_pending']==0
        inject(app,REPORT); assert ADDRESS not in [m['radiator'] for m in status(app)['monitors']]
        no_tx(app)
    finally: app.stop()


def interrupted(directory, reboot=False, fast=False):
    storage=directory/('outbox'+str(reboot)+str(fast)); app=App(storage,operating_mode=None)
    try:
        inject(app,REPORT); select(app); setup(app)
        app.sim('mqtt_confirmed_mode',mode=2); enable(app); c=configs(app,500)
        assert len(c)==1 and app.http('/api/simulation/mqtt-client')['outbox_pending']==1
        app.sim('mqtt_connection',connected=False)
        if fast: app.sim('mqtt_connection',connected=True)
        select(app,action='remove'); app.advance(100)
        if reboot:
            app.stop(); app=App(storage,operating_mode=None)
            app.sim('network_event',name='started'); app.sim('network_event',name='got_ip'); app.advance(100)
        else:
            state=app.http('/api/simulation/mqtt-client')
            assert state['outbox_discarded']==1 and not state['outbox_pending']
        app.sim('mqtt_confirmed_mode',mode=0); app.advance(5100); connect(app)
        c=configs(app); assert c and all(all(set(x)=={'platform'} for x in r['payload']['components'].values()) for r in c)
        app.advance(100); assert not app.http('/api/mqtt')['discovery_pending']
        app.sim('mqtt_replay_outbox'); assert not configs(app,100)
        assert app.http('/api/simulation/mqtt-client')['replayed']==0
        no_tx(app)
    finally: app.stop()


def cleanup_interrupted(directory):
    storage=directory/'cleanup-ack'; app=App(storage,operating_mode=None)
    try:
        inject(app,REPORT); select(app); setup(app); enable(app); configs(app); app.advance(100)
        app.sim('mqtt_confirmed_mode',mode=2); select(app,action='remove')
        c=configs(app,5000); assert c and all(set(x)=={'platform'} for x in c[-1]['payload']['components'].values())
        assert app.http('/api/simulation/mqtt-client')['outbox_pending']==1
        assert app.http('/api/mqtt')['discovery_pending']
        no_tx(app)
    finally: app.stop()
    app=App(storage,operating_mode=None)
    try:
        app.sim('network_event',name='started'); app.sim('network_event',name='got_ip'); app.advance(100); connect(app)
        c=configs(app); assert c and all(set(x)=={'platform'} for x in c[-1]['payload']['components'].values())
        app.advance(100); assert not app.http('/api/mqtt')['discovery_pending']; no_tx(app)
    finally: app.stop()


def failures(directory):
    storage=directory/'mode-failure'; app=App(storage,operating_mode=None,mode_storage_fault=2)
    try:
        inject(app,REPORT); select(app)
        s=status(app); assert s['monitor_save_status']=='failed' and s['monitor_storage_error'] and s['monitor_persistence_uncertain']
        assert not s['monitors'] and not Path(str(storage)+'.ha_monitors').exists()
        assert not Path(str(storage)+'.rf_mode').exists(); no_tx(app)
    finally: app.stop()
    app=App(storage,operating_mode=None)
    try: assert status(app)['mode']=='listen-only' and not status(app)['monitors']; no_tx(app)
    finally: app.stop()
    storage=directory/'write-failure'; app=App(storage,operating_mode=None)
    try:
        inject(app,REPORT); select(app)
        path=Path(str(storage)+'.ha_monitors'); old=path.read_bytes(); path.unlink(); path.mkdir()
        select(app,name='Not saved',action='rename')
        s=status(app); assert s['monitor_save_status']=='failed' and s['monitor_persistence_uncertain']
        assert s['monitors'][0]['name']=='Passive room'; no_tx(app)
        path.rmdir(); path.write_bytes(old)
    finally: app.stop()
    app=App(storage,operating_mode=None)
    try: assert status(app)['monitors'][0]['name']=='Passive room' and not status(app)['monitor_storage_error']; no_tx(app)
    finally: app.stop()


def coexist(directory):
    storage=directory/'coexist'; app=App(storage,operating_mode=None)
    try:
        inject(app,REPORT); select(app); mode(app,'control-enabled'); no_tx(app)
        app.http('/api/pairing',dict(action='start',source='F1010203',destination='F1100001'),202); app.advance(2500)
        s=app.http(); assert s['inclusion']['error']==0x7306 and not s['radiators']
        assert len(s['simulation_tx'])==1 # explicit inclusion offer, no bind
        select(app,action='remove')
        app.http('/api/pairing',dict(action='start',source='F1010203',destination='F1100001'),202); app.advance(2500)
        assert app.http()['inclusion']['saved']
        select(app,code=409)
        app.http('/api/radiators',dict(action='remove',id=1),202); app.advance(1)
        assert not status(app)['monitors'] # removal never selects implicitly
    finally: app.stop()


def payload_limit(directory):
    app=App(directory/'limit-monitor',operating_mode=None)
    try:
        inject(app,REPORT); select(app); setup(app); enable(app)
        assert not configs(app) and app.http('/api/mqtt')['discovery_error']==0x7412
        no_tx(app)
    finally: app.stop()


if __name__=='__main__':
    with tempfile.TemporaryDirectory() as d:
        d=Path(d)
        if os.environ.get('CLEVERBOX_DISCOVERY_LIMIT_TEST'): payload_limit(d)
        else:
            run(d); failures(d); coexist(d); cleanup_interrupted(d)
            interrupted(d); interrupted(d,fast=True); interrupted(d,reboot=True)
    print('Passive HA: address identities, bounded dedicated receive storage, persistence, mode gate, freshness, cleanup/outbox/reboot, coexistence and zero TX: passed')
