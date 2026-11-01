#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Real HA Core 2026.10.4 + Mosquitto + production host application.

No fake HA entities/MQTT client: load the shipped YAML package, configure HA's
built-in MQTT integration, call helper/button services and inspect real states.
Only Wi-Fi, UTC sync and RF hardware boundaries are simulated. Real-time HA
snapshot expiry is tested without clock mocking; RF deadlines are accelerated.
"""
import json
import os
from pathlib import Path
import pwd
import select
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import paho.mqtt.client as mqtt
from test_http import seed_pairing, App, ROOT, REPORT, changed_frame
from test_registry import REPORT_B
from test_passive import seal
from test_mqtt import BASE


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


class HomeAssistant:
    def __init__(self, directory, broker_port):
        self.log = open(directory / 'worker.log', 'ab')
        self.process = subprocess.Popen([sys.executable, str(ROOT / 'host/ha_worker.py'), str(directory), str(broker_port)],
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.log, text=True)
        try:
            assert self.read(180) == {'ready': '2026.10.4'}
        except Exception:
            print((directory / 'worker.log').read_text()[-12000:], file=sys.stderr)
            self.stop()
            raise

    def read(self, timeout=20):
        until = time.monotonic() + timeout
        while time.monotonic() < until:
            if select.select([self.process.stdout], [], [], .1)[0]:
                line = self.process.stdout.readline()
                assert line, 'HA exited; inspect worker.log/home-assistant.log'
                if line.startswith('{'):
                    return json.loads(line)
            assert self.process.poll() is None, 'HA exited; inspect worker.log/home-assistant.log'
        raise AssertionError('HA RPC timeout; inspect worker.log/home-assistant.log')

    def rpc(self, **request):
        self.process.stdin.write(json.dumps(request) + '\n'); self.process.stdin.flush()
        result = self.read()
        assert 'error' not in result, result
        return result

    def states(self):
        return self.rpc(op='states')

    def call(self, domain, service, **data):
        return self.rpc(op='call', domain=domain, service=service, data=data)

    def stop(self):
        if self.process.poll() is None:
            self.process.stdin.write('{"op":"stop"}\n'); self.process.stdin.flush()
            try: self.process.wait(timeout=20)
            except subprocess.TimeoutExpired: self.process.kill(); self.process.wait()
        self.process.stdin.close(); self.process.stdout.close(); self.log.close()


def run(directory):
    directory = Path(directory)
    config = directory / 'ha'; (config / 'packages').mkdir(parents=True)
    shutil.copy(ROOT / 'examples/home-assistant/cleverbox.yaml', config / 'packages')
    (config / 'configuration.yaml').write_text(f'''homeassistant:
  name: Cleverbox integration test
  latitude: 0
  longitude: 0
  elevation: 0
  time_zone: UTC
  unit_system: metric
  packages: !include_dir_named packages
frontend:
http:
  server_host: 127.0.0.1
  server_port: {free_port()}
''')
    # The actual HA configuration checker validates the complete shipped package.
    with open(directory / 'check-config.log', 'w') as check_log:
        subprocess.run([sys.executable, '-m', 'homeassistant', '--script', 'check_config', '--config', str(config)],
                       stdout=check_log, stderr=check_log, check=True, timeout=90)
    port = free_port()
    conf = directory / 'mosquitto.conf'
    conf.write_text(f'listener {port} 127.0.0.1\nallow_anonymous true\npersistence true\npersistence_location {directory}/\nautosave_interval 1\nuser {pwd.getpwuid(os.geteuid()).pw_name}\n')
    broker_log = open(directory / 'broker.log', 'wb')
    def start_broker():
        process = subprocess.Popen([shutil.which('mosquitto'), '-c', str(conf)], stdout=broker_log, stderr=broker_log)
        until = time.monotonic() + 5
        while True:
            try:
                with socket.create_connection(('127.0.0.1', port), timeout=.1): return process
            except OSError:
                assert process.poll() is None and time.monotonic() < until
                time.sleep(.02)
    broker = start_broker()
    storage = directory / 'bridge.bin'; seed_pairing(storage)
    app = App(storage, real_mqtt=True); ha = None
    observer = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id='ha-test-observer', clean_session=True)
    messages = []
    observer.on_connect = lambda c,u,f,r,p: c.subscribe([(BASE+'/#',0),('homeassistant/#',0)])
    def received(c,u,m):
        value = m.payload.decode()
        messages.append((m.topic, json.loads(value) if value.startswith('{') else value, m.retain))
    observer.on_message = received; observer.connect('127.0.0.1', port, 30)
    def advance(ms):
        # Real socket I/O runs between HTTP requests. Do not jump beyond the
        # production ACK timeout while starving that boundary of poll turns.
        # RF/report deadlines still advance faster than wall time.
        while ms > 0:
            step=min(ms,100)
            app.advance(step); ms-=step

    def pump(predicate=lambda: True, timeout=10, step=100):
        until = time.monotonic()+timeout
        while time.monotonic() < until:
            advance(step); observer.loop(timeout=.002)
            if predicate(): return
            time.sleep(.02)
        raise AssertionError(('condition timed out', messages[-8:], ha.states() if ha else None))
    def state(entity):
        return ha.states().get(entity, {}).get('state')
    def state_is(entity, value):
        return state(entity) == value
    def session():
        return app.http('/api/mqtt')['session']
    def snapshots():
        advance(5100)
        pump(lambda: state_is('sensor.cleverbox_session', session()) and
             all(state_is(f'sensor.cleverbox_{i}_snapshot', session()) for i in (1,2)))
        # Ensure attributes, not only the unchanged session-valued state, arrived.
        time.sleep(.15)
    def apply(i, target=None, mode=None):
        if target is not None: ha.call('input_number','set_value',entity_id=f'input_number.cleverbox_{i}_draft_target',value=target)
        if mode is not None: ha.call('input_select','select_option',entity_id=f'input_select.cleverbox_{i}_draft_mode',option=mode)
        ha.call('button','press',entity_id=f'button.cleverbox_{i}_apply')
    def admitted(i, reason):
        return state_is(f'sensor.cleverbox_020000000001_{i}_last_admission', reason)
    def phase(i, value):
        return app.http(f'/api/radio?id={i}')['command']['phase'] == value
    try:
        app.sim('device', index=1)
        app.http('/api/pairing', dict(action='start',source='F1010203',destination='F1100001'),202); advance(2000)
        assert len(app.http()['radiators']) == 2
        app.http('/api/mqtt',dict(action='configure',host='127.0.0.1',port=port,username='',password='',enabled=True),202)
        advance(100)
        app.http('/api/mqtt',dict(action='discovery_enable'),202)
        advance(100)
        app.http('/api/network',dict(action='configure',ssid='synthetic',password='synthetic'),202)
        app.sim('network_event',name='started'); app.sim('network_event',name='got_ip')
        pump(lambda:app.http('/api/mqtt')['connected'])
        tx = app.http()['simulation_tx']
        ha = HomeAssistant(config,port); snapshots()
        pump(lambda:state_is('sensor.cleverbox_020000000001_1_temperature','unavailable'))
        def registry(): return ha.rpc(op='registries')
        def discovered():
            return [e for e in registry()['entities'] if e['unique_id'].startswith('cleverbox_020000000001_') and
                    e['unique_id'].rsplit('_',1)[-1] in ('c','mode','fresh','age','admission','delivery')]
        pump(lambda:len(discovered()) == 14)
        entries = {e['unique_id']:e for e in discovered()}
        assert len({e['device_id'] for e in entries.values()}) == 2
        for i in (1,2):
            assert len({e['device_id'] for uid,e in entries.items() if uid.startswith(f'cleverbox_020000000001_{i}_')}) == 1
        app.http('/api/radiators',dict(action='rename',id=1,name='Living room renamed'),202)
        pump(lambda:any(d['name']=='Living room renamed' for d in registry()['devices']))
        assert {e['unique_id']:e for e in discovered()} == entries
        assert app.http()['simulation_tx'] == tx
        print('HA boot/package validation: passed', flush=True)
        # HA Apply cannot bypass listen-only; unrelated passive reports never
        # become discovery devices. Exercise the real broker/HA service path.
        app.http('/api/commissioning', {'mode':'listen-only'}, 202); advance(10)
        snapshots(); apply(1,22,'Comfort'); pump(lambda:admitted(1,'listen_only'))
        assert app.http()['simulation_tx'] == tx
        app.sim('inject',frame=changed_frame(REPORT,3)); advance(10)
        assert app.http('/api/commissioning')['observations']
        snapshots(); assert len(discovered()) == 14
        app.http('/api/commissioning', {'mode':'control-enabled'}, 202); advance(10)
        assert app.http()['simulation_tx'] == tx
        snapshots()
        print('HA listen-only rejection, passive discovery isolation and silent explicit enable: passed', flush=True)
        apply(1,22,'Comfort'); pump(lambda:admitted(1,'time_unavailable'))
        assert app.http()['simulation_tx'] == tx
        app.http('/api/time',dict(action='manual',unix_seconds=1767268800)); snapshots()
        assert all('template_available' not in d['command'] for d in app.http()['radiators'])
        # Actual HA draft-and-Apply flow requires no template on either radiator.
        apply(1,22,'Comfort'); pump(lambda:phase(1,'matched'))
        snapshots(); pump(lambda:state_is('sensor.cleverbox_020000000001_1_observed_target','22.0'))
        assert state_is('sensor.cleverbox_020000000001_1_observed_mode','Comfort')
        assert float(state('sensor.cleverbox_020000000001_1_temperature')) == 20
        # Target draft persists while mode alone changes; neither edit publishes.
        before = len([t for t,p,r in messages if t.endswith('/set')])
        ha.call('input_select','select_option',entity_id='input_select.cleverbox_1_draft_mode',option='Eco')
        pump(); assert len([t for t,p,r in messages if t.endswith('/set')]) == before
        apply(1); pump(lambda:phase(1,'matched') and app.http('/api/radio?id=1')['observed']['mode']==3)
        snapshots(); pump(lambda:state_is('sensor.cleverbox_020000000001_1_observed_mode','Eco'))
        assert state_is('sensor.cleverbox_020000000001_1_observed_target','22.0')
        print('HA no-template draft/Apply controls, full commands, prerequisites and observations: passed', flush=True)
        # Two Apply actions before a new token: exactly one admission/RF poll.
        app.sim('scenario',name='silence_a'); snapshots()
        apply(1,23,'Comfort'); apply(1,24,'Eco')
        pump(lambda:admitted(1,'invalid_command_token'))
        assert state_is('sensor.cleverbox_020000000001_1_observed_target','22.0')
        assert app.http('/api/radio?id=1')['command']['requested_target_c']==23
        snapshots(); apply(1)
        pump(lambda:admitted(1,'busy_or_unavailable'))
        apply(2,16,'Eco'); pump(lambda:phase(2,'matched'))
        assert app.http()['radiators'][0]['command']['pending']
        snapshots(); pump(lambda:state_is('sensor.cleverbox_020000000001_2_observed_target','16.0'))
        # Delivery timeout is a separate historical entity, not an observation.
        advance(151000); pump(lambda:state_is('sensor.cleverbox_020000000001_1_last_delivery','timeout'))
        assert app.http('/api/radio?id=1')['command']['attempts']==0
        advance(305000); snapshots()
        pump(lambda:state_is('sensor.cleverbox_020000000001_1_temperature','unavailable'))
        assert state_is('sensor.cleverbox_020000000001_2_temperature','unavailable')
        app.sim('inject',frame=REPORT); app.sim('inject',frame=REPORT_B); advance(10); snapshots()
        pump(lambda:state_is('binary_sensor.cleverbox_020000000001_1_observation_fresh','on'))
        # A fresh report can explicitly lack the measured-temperature field.
        missing=bytearray.fromhex(REPORT); missing[16:18]=bytes.fromhex('084C'); missing[16]^=1
        app.sim('inject',frame=changed_frame(missing.hex(),16)); advance(10); snapshots()
        pump(lambda:state_is('sensor.cleverbox_020000000001_1_temperature','unavailable'))
        assert state_is('binary_sensor.cleverbox_020000000001_1_observation_fresh','on')
        assert state_is('sensor.cleverbox_020000000001_1_observed_target','21.0')
        app.sim('inject',frame=REPORT); advance(10); snapshots()
        print('HA rapid actions, busy rejection, two radiators, RF timeout, stale/missing measurements and recovery: passed', flush=True)
        # Missing snapshots expire in real HA wall time despite online availability.
        # --test-clock freezes publication while HA's native expire_after runs.
        until = time.monotonic()+18
        while time.monotonic()<until:
            observer.loop(timeout=.05)
            if state_is('sensor.cleverbox_1_snapshot','unavailable'): break
            time.sleep(.1)
        assert state_is('sensor.cleverbox_1_snapshot','unavailable')
        assert state_is('sensor.cleverbox_020000000001_1_temperature','unavailable')
        assert state_is('button.cleverbox_1_apply','unavailable')
        snapshots(); pump(lambda:state_is('binary_sensor.cleverbox_020000000001_1_observation_fresh','on'))
        # HA restart preserves MQTT entry but never restores/replays an action.
        tx = app.http()['simulation_tx']; original_session = session()
        last_id = max(p['request_id'] for t,p,r in messages if isinstance(p,dict) and p.get('stage')=='admission' and p.get('accepted'))
        ha.stop(); ha = HomeAssistant(config,port); snapshots()
        assert session()==original_session and app.http()['simulation_tx']==tx
        apply(2,17,'Frost'); pump(lambda:phase(2,'matched') and app.http()['radiators'][1]['observed']['mode']==2)
        pump(lambda: ha.states()['sensor.cleverbox_020000000001_2_last_admission']['attributes']['result']['request_id'] > last_id)
        print('HA missing-snapshot expiry, recovery and process restart without replay: passed', flush=True)
        # Admit RF then remove the broker. Delivery proceeds during network loss.
        snapshots(); app.sim('scenario',name='delayed'); apply(2,18,'Comfort')
        pump(lambda:app.http()['radiators'][1]['command']['pending'],step=100)
        broker.terminate(); broker.wait(timeout=5)
        advance(6000); assert phase(2,'matched')
        tx = app.http()['simulation_tx']; broker = start_broker(); observer.reconnect()
        pump(lambda:app.http('/api/mqtt')['connected'] and session()!=original_session,timeout=20)
        changed_session = session(); snapshots(); assert session()==changed_session
        assert app.http()['simulation_tx']==tx
        pump(lambda:state_is('sensor.cleverbox_020000000001_2_last_delivery','matched'))
        result = ha.states()['sensor.cleverbox_020000000001_2_last_delivery']['attributes']['result']
        assert result['session']==original_session
        apply(1,20,'Off'); pump(lambda:phase(1,'matched') and app.http('/api/radio?id=1')['observed']['mode']==1)
        pump(lambda: ha.states()['sensor.cleverbox_020000000001_1_last_admission']['attributes']['result']['session'] == changed_session)
        assert ha.states()['sensor.cleverbox_020000000001_1_last_admission']['attributes']['result']['request_id'] == 1
        # Real HA observes radio loss separately from quiet radiators. Retain
        # diagnostic ages/results; disable readings and Apply without deleting IDs.
        snapshots(); tx=app.http()['simulation_tx']
        age_before=app.http('/api/radio?id=1')['observed']['age_ms']
        app.sim('radio_fault',stage='initialize',code=-42)
        app.sim('radio_fault',stage='spi',code=-61); advance(1)
        app.sim('radio_fault',stage='spi',code=0); advance(10000)
        assert app.http()['health']=='faulted'
        pump(lambda:state_is('sensor.cleverbox_020000000001_1_temperature','unavailable') and
             state_is('binary_sensor.cleverbox_020000000001_1_observation_fresh','off') and
             state_is('binary_sensor.cleverbox_ready','off'))
        pump(lambda:ha.states()['binary_sensor.cleverbox_020000000001_1_observation_fresh']['attributes'].get('radio_ready') is False)
        assert ha.states()['sensor.cleverbox_020000000001_1_report_age']['state']!='unavailable'
        assert state_is('button.cleverbox_1_apply','unavailable')
        app.sim('radio_fault',stage='initialize',code=0)
        app.http('/api/radio',dict(action='retry'),202); advance(100)
        pump(lambda:state_is('binary_sensor.cleverbox_ready','on') and
             not state_is('sensor.cleverbox_020000000001_1_temperature','unavailable'))
        assert app.http('/api/radio?id=1')['observed']['age_ms']>=age_before+10000
        assert app.http()['simulation_tx']==tx
        # Restart actual bridge process with a pending command. Boot cannot resume it.
        snapshots(); app.sim('scenario',name='silence'); apply(2,19,'Eco')
        pump(lambda:app.http()['radiators'][1]['command']['pending'])
        old_command = [p for t,p,r in messages if t.endswith('/2/set')][-1]
        app.stop(); app = App(storage,real_mqtt=True)
        app.sim('network_event',name='started'); app.sim('network_event',name='got_ip')
        pump(lambda:app.http('/api/mqtt')['connected'])
        assert session()!=changed_session and app.http()['simulation_tx']==[]
        assert not app.http('/api/time')['usable'] and all(r['command']['phase']=='idle' for r in app.http()['radiators'])
        snapshots(); assert state_is('sensor.cleverbox_020000000001_1_temperature','unavailable')
        observer.publish(BASE+'/radiators/2/set',json.dumps(old_command))
        pump(lambda:admitted(2,'session_mismatch'))
        assert app.http()['simulation_tx']==[]
        app.http('/api/time',dict(action='manual',unix_seconds=1767268800)); snapshots()
        apply(2,19,'Eco'); pump(lambda:phase(2,'matched'))
        pump(lambda: ha.states()['sensor.cleverbox_020000000001_2_last_admission']['attributes']['result']['session'] == session())
        assert ha.states()['sensor.cleverbox_020000000001_2_last_admission']['attributes']['result']['request_id'] == 1
        sets = [(p,r) for t,p,r in messages if t.endswith('/set')]
        assert sets and not any(retain for _,retain in sets)
        assert all(set(p)=={'session','command_token','mode','centi_c'} for p,_ in sets)
        print('HA broker/bridge restarts, new sessions, RF continuity and no reconnect replay: passed',flush=True)
        # Online disable/enable cleanup, HA birth replay and unrelated broker data.
        unrelated='homeassistant/sensor/cleverbox_test_unrelated/config'
        other=dict(name='Unrelated test',unique_id='unrelated_test',state_topic='unrelated/state')
        observer.publish(unrelated,json.dumps(other),retain=True,qos=1)
        tx=app.http()['simulation_tx']
        before=len([t for t,p,r in messages if t.startswith('homeassistant/device/')])
        observer.publish('homeassistant/status','online')
        pump(lambda:len([t for t,p,r in messages if t.startswith('homeassistant/device/')])>before)
        assert app.http()['simulation_tx']==tx
        app.http('/api/mqtt',dict(action='discovery_disable'),202); advance(100)
        pump(lambda:not discovered(),timeout=20)
        pump(lambda:app.http('/api/mqtt')['discovery_pending']==0)
        assert app.http()['simulation_tx']==tx
        app.http('/api/mqtt',dict(action='discovery_enable'),202); advance(100)
        pump(lambda:len(discovered())==14,timeout=20)
        assert {e['unique_id'] for e in discovered()}==set(entries)
        assert app.http()['simulation_tx']==tx
        # Keep retained configs across reconnect while removing a radiator offline.
        broker.terminate(); broker.wait(timeout=5)
        advance(6000)
        app.http('/api/radiators',dict(action='remove',id=1),202); advance(10)
        app.stop(); app = App(storage,real_mqtt=True)
        app.sim('network_event',name='started'); app.sim('network_event',name='got_ip')
        tx = app.http()['simulation_tx']
        broker = start_broker(); observer.reconnect()
        pump(lambda:app.http('/api/mqtt')['connected'],timeout=20)
        pump(lambda:not any(e['unique_id'].startswith('cleverbox_020000000001_1_') for e in discovered()),timeout=20)
        pump(lambda:len(discovered()) == 7)
        assert app.http()['simulation_tx'] == tx
        # Registry addition after discovery was enabled uses a new stable ID.
        app.sim('device',index=0)
        added=app.http('/api/pairing',dict(action='start',source='F1010203',destination='F1100001'),202)['id']
        assert added==3
        advance(2000)
        pump(lambda:len(discovered())==14,timeout=20)
        assert len({e['device_id'] for e in discovered()})==2
        assert any(e['unique_id']=='cleverbox_020000000001_3_temperature_centi_c' for e in discovered())
        assert state_is('sensor.cleverbox_020000000001_3_temperature','unavailable')
        broker.terminate(); broker.wait(timeout=5); advance(6000)
        app.http('/api/mqtt',dict(action='discovery_disable'),202); advance(200)
        assert not app.http('/api/mqtt')['discovery_enabled']
        app.stop(); app = App(storage,real_mqtt=True)
        app.sim('network_event',name='started'); app.sim('network_event',name='got_ip')
        broker = start_broker(); observer.reconnect()
        pump(lambda:app.http('/api/mqtt')['connected'],timeout=20)
        pump(lambda:not discovered(),timeout=20)
        assert not app.http('/api/mqtt')['discovery_enabled'] and app.http()['simulation_tx']==[]
        # A fresh subscription still receives unrelated retained data after cleanup.
        messages.clear(); observer.subscribe(unrelated)
        pump(lambda:any(t==unrelated and p==other and retained for t,p,retained in messages))
        ha.stop(); ha = HomeAssistant(config,port)
        pump(lambda:not discovered())
        assert app.http()['simulation_tx']==[]
        print('HA stable discovery IDs/grouping, rename/addition, offline removal/disable and durable cleanup markers: passed',flush=True)
        # Dedicated passive milestone: real MQTT discovery and actual HA entity
        # runtime, with independent report/bridge-publication expiry clocks.
        app.http('/api/commissioning',dict(mode='listen-only'),202); advance(10)
        tx=app.http()['simulation_tx']; assert tx==[]
        address='F1445566'; prefix='cleverbox_020000000001_passive_'+address
        entity_prefix=prefix.lower()
        sensor='sensor.'+entity_prefix+'_temperature'
        fresh='binary_sensor.'+entity_prefix+'_observation_fresh'
        def passive_entries(): return [e for e in registry()['entities'] if e['unique_id'].startswith(prefix+'_')]
        def report(controller='D0123456',missing=False):
            f=bytearray.fromhex(REPORT); f[1:5]=bytes.fromhex(address); f[6:10]=bytes.fromhex(controller)
            if missing: f[16:18]=bytes.fromhex('084C'); f[19:21]=bytes.fromhex('084C')
            app.sim('inject',frame=seal(f)); advance(10)
        def monitor(action,name='Observed room'):
            body=dict(action=action,radiator=address)
            if action!='remove': body['name']=name
            app.http('/api/commissioning',body,202); advance(10)
            assert app.http('/api/commissioning')['monitor_save_status']=='succeeded'
        report(); monitor('select')
        assert not passive_entries() # Global discovery is still disabled.
        app.http('/api/mqtt',dict(action='discovery_enable'),202); advance(100)
        pump(lambda:len(passive_entries())==5,timeout=20)
        pump(lambda:state_is(sensor,'20.0'))
        passive_ids={e['unique_id'] for e in passive_entries()}
        assert len({e['device_id'] for e in passive_entries()})==1
        assert all(e['entity_id'].startswith(('sensor.','binary_sensor.')) for e in passive_entries())
        monitor('rename','Renamed observation')
        pump(lambda:any(d['name']=='Renamed observation' for d in registry()['devices']))
        assert {e['unique_id'] for e in passive_entries()}==passive_ids
        report('F1998877'); advance(5100)
        pump(lambda:ha.states().get('sensor.'+entity_prefix+'_report_age',{}).get('attributes',{}).get('controller')=='F1998877')
        # Selected address and original age survive exhausted radio recovery.
        original_age=app.http('/api/commissioning')['monitors'][0]['age_ms']
        app.sim('radio_fault',stage='initialize',code=-42)
        app.sim('radio_fault',stage='spi',code=-61); advance(1)
        app.sim('radio_fault',stage='spi',code=0); advance(10000)
        pump(lambda:state_is(sensor,'unavailable') and state_is(fresh,'off'))
        pump(lambda:ha.states()[fresh]['attributes'].get('radio_health')=='faulted')
        assert {e['unique_id'] for e in passive_entries()}==passive_ids
        assert ha.states()['sensor.'+entity_prefix+'_report_age']['state']!='unavailable'
        app.sim('radio_fault',stage='initialize',code=0)
        app.http('/api/radio',dict(action='retry'),202); advance(100)
        pump(lambda:state_is(sensor,'20.0') and state_is(fresh,'on'))
        assert app.http('/api/commissioning')['monitors'][0]['age_ms']>=original_age+10000
        assert app.http()['simulation_tx']==tx
        report(missing=True); advance(5100)
        pump(lambda:state_is(sensor,'unavailable'))
        assert state_is('sensor.'+entity_prefix+'_observed_target','unavailable') and state_is(fresh,'on')
        report(); advance(5100); pump(lambda:state_is(sensor,'20.0'))
        advance(300000); pump(lambda:state_is(sensor,'unavailable') and state_is(fresh,'off'))
        report(); advance(5100); pump(lambda:state_is(sensor,'20.0'))
        # No simulation advance or bridge publications while HA wall time runs.
        until=time.monotonic()+20
        while time.monotonic()<until and not state_is(sensor,'unavailable'):
            observer.loop(timeout=.05); time.sleep(.1)
        assert state_is(sensor,'unavailable') and state_is(fresh,'unavailable')
        advance(5100); pump(lambda:state_is(sensor,'20.0'))
        assert app.http()['simulation_tx']==tx
        # HA restart retains identity and gets fresh RAM observations on birth.
        ha.stop(); ha=HomeAssistant(config,port)
        pump(lambda:state_is(sensor,'20.0'),timeout=30)
        assert {e['unique_id'] for e in passive_entries()}==passive_ids
        # Bridge restart retains the selection, publishes no persisted reading.
        app.stop(); app=App(storage,real_mqtt=True,operating_mode=None)
        app.sim('network_event',name='started'); app.sim('network_event',name='got_ip')
        pump(lambda:app.http('/api/mqtt')['connected'],timeout=20)
        pump(lambda:state_is(sensor,'unavailable') and state_is(fresh,'off'))
        assert app.http('/api/commissioning')['mode']=='listen-only'
        assert {e['unique_id'] for e in passive_entries()}==passive_ids
        report(); advance(5100); pump(lambda:state_is(sensor,'20.0'))
        # Broker/HA reconnect can use the HA client's reconnect backoff. Wait for
        # actual observed state, not merely the bridge's earlier SUBACK.
        old_session=session(); broker.terminate(); broker.wait(timeout=5); advance(6000)
        broker=start_broker(); observer.reconnect()
        pump(lambda:app.http('/api/mqtt')['connected'] and session()!=old_session,timeout=30)
        report(); advance(5100); pump(lambda:state_is(sensor,'20.0'),timeout=30)
        app.http('/api/mqtt',dict(action='discovery_disable'),202); advance(100)
        pump(lambda:not passive_entries(),timeout=20)
        assert len(app.http('/api/commissioning')['monitors'])==1
        app.http('/api/mqtt',dict(action='discovery_enable'),202); advance(100)
        pump(lambda:len(passive_entries())==5,timeout=20)
        assert {e['unique_id'] for e in passive_entries()}==passive_ids
        # Remove while offline, restart bridge before cleanup, then restart HA
        # after retained component-removal markers have been acknowledged.
        broker.terminate(); broker.wait(timeout=5); advance(6000); monitor('remove')
        app.stop(); app=App(storage,real_mqtt=True,operating_mode=None)
        app.sim('network_event',name='started'); app.sim('network_event',name='got_ip')
        broker=start_broker(); observer.reconnect()
        pump(lambda:app.http('/api/mqtt')['connected'],timeout=30)
        pump(lambda:not passive_entries(),timeout=30)
        pump(lambda:app.http('/api/mqtt')['discovery_pending']==0)
        ha.stop(); ha=HomeAssistant(config,port); pump(lambda:not passive_entries())
        assert not app.http('/api/commissioning')['monitors'] and app.http()['simulation_tx']==[]
        print('HA passive entities: selection/rename/identity, controller changes, missing readings, monotonic freshness, actual wall-time expiry, HA/bridge/broker restarts, disable/re-enable, durable offline cleanup and zero RF: passed',flush=True)

    finally:
        if ha: ha.stop()
        app.stop(); observer.disconnect()
        broker.terminate(); broker.wait(timeout=5); broker_log.close()


if __name__ == '__main__':
    assert shutil.which('mosquitto'), 'Install Mosquitto 2.x first'
    # Keep logs for reproducibility when explicitly requested; otherwise temporary.
    if path := os.environ.get('CLEVERBOX_HA_TEST_DIR'):
        Path(path).mkdir(parents=True,exist_ok=True); run(path)
    else:
        with tempfile.TemporaryDirectory() as directory: run(directory)
    print('Actual Home Assistant Core 2026.10.4 / Mosquitto integration: passed')
