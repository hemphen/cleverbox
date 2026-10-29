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
from test_http import App, ROOT, STATE, REPORT
from test_registry import REPORT_B
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
        assert self.read(90) == {'ready': '2026.10.4'}

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
    conf.write_text(f'listener {port} 127.0.0.1\nallow_anonymous true\npersistence false\nuser {pwd.getpwuid(os.geteuid()).pw_name}\n')
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
    storage = directory / 'bridge.bin'; storage.write_bytes(bytes.fromhex('01F1010203F1040506'))
    app = App(storage, real_mqtt=True); ha = None
    observer = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id='ha-test-observer', clean_session=True)
    messages = []
    observer.on_connect = lambda c,u,f,r,p: c.subscribe(BASE+'/#')
    def received(c,u,m):
        value = m.payload.decode()
        messages.append((m.topic, json.loads(value) if value.startswith('{') else value, m.retain))
    observer.on_message = received; observer.connect('127.0.0.1', port, 30)
    def pump(predicate=lambda: True, timeout=10, step=100):
        until = time.monotonic()+timeout
        while time.monotonic() < until:
            app.advance(step); observer.loop(timeout=.002)
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
        app.advance(5100)
        pump(lambda: state_is('sensor.cleverbox_session', session()) and
             all(state_is(f'sensor.cleverbox_{i}_snapshot', session()) for i in (1,2)))
        # Ensure attributes, not only the unchanged session-valued state, arrived.
        time.sleep(.15)
    def apply(i, target=None, mode=None):
        if target is not None: ha.call('input_number','set_value',entity_id=f'input_number.cleverbox_{i}_draft_target',value=target)
        if mode is not None: ha.call('input_select','select_option',entity_id=f'input_select.cleverbox_{i}_draft_mode',option=mode)
        ha.call('button','press',entity_id=f'button.cleverbox_{i}_apply')
    def admitted(i, reason):
        return state_is(f'sensor.cleverbox_{i}_last_admission', reason)
    def phase(i, value):
        return app.http(f'/api/radio?id={i}')['command']['phase'] == value
    try:
        app.sim('device', index=1)
        app.http('/api/pairing', dict(action='start',source='F1010203',destination='F1100001'),202); app.advance(2000)
        assert len(app.http()['radiators']) == 2
        app.http('/api/mqtt',dict(action='configure',host='127.0.0.1',port=port,username='',password='',enabled=True),202)
        app.http('/api/network',dict(action='configure',ssid='synthetic',password='synthetic'),202)
        app.sim('network_event',name='started'); app.sim('network_event',name='got_ip')
        pump(lambda:app.http('/api/mqtt')['connected'])
        tx = app.http()['simulation_tx']
        ha = HomeAssistant(config,port); snapshots()
        assert state_is('sensor.cleverbox_1_temperature','unavailable')
        assert app.http()['simulation_tx'] == tx
        print('HA boot/package validation: passed', flush=True)
        apply(1,22,'Comfort'); pump(lambda:admitted(1,'time_unavailable'))
        assert app.http()['simulation_tx'] == tx
        app.http('/api/time',dict(action='manual',unix_seconds=1767268800)); snapshots()
        apply(1); pump(lambda:admitted(1,'busy_or_unavailable_or_missing_template'))
        assert app.http()['simulation_tx'] == tx
        app.http('/api/controller-state',dict(action='import',id=1,state=STATE),202); app.advance(10)
        app.sim('fixture'); app.advance(10) # B was selected explicitly above.
        app.http('/api/controller-state',dict(action='save',id=2),202); app.advance(10)
        snapshots()
        apply(1,22,'Comfort'); pump(lambda:phase(1,'matched'))
        snapshots(); pump(lambda:state_is('sensor.cleverbox_1_observed_target','22.0'))
        assert state_is('sensor.cleverbox_1_observed_mode','Comfort')
        assert float(state('sensor.cleverbox_1_temperature')) == 20
        # Target draft persists while mode alone changes; neither edit publishes.
        before = len([t for t,p,r in messages if t.endswith('/set')])
        ha.call('input_select','select_option',entity_id='input_select.cleverbox_1_draft_mode',option='Eco')
        pump(); assert len([t for t,p,r in messages if t.endswith('/set')]) == before
        apply(1); pump(lambda:phase(1,'matched') and app.http()['observed']['mode']==3)
        snapshots(); pump(lambda:state_is('sensor.cleverbox_1_observed_mode','Eco'))
        assert state_is('sensor.cleverbox_1_observed_target','22.0')
        print('HA controls, full commands, prerequisites and observations: passed', flush=True)
        # Two Apply actions before a new token: exactly one admission/RF poll.
        app.sim('scenario',name='silence_a'); snapshots()
        apply(1,23,'Comfort'); apply(1,24,'Eco')
        pump(lambda:admitted(1,'invalid_command_token'))
        assert app.http()['command']['requested_target_c']==23
        snapshots(); apply(1)
        pump(lambda:admitted(1,'busy_or_unavailable_or_missing_template'))
        apply(2,16,'Eco'); pump(lambda:phase(2,'matched'))
        assert app.http()['radiators'][0]['command']['pending']
        snapshots(); pump(lambda:state_is('sensor.cleverbox_2_observed_target','16.0'))
        # Delivery timeout is a separate historical entity, not an observation.
        app.advance(151000); pump(lambda:state_is('sensor.cleverbox_1_last_delivery','timeout'))
        assert app.http()['command']['attempts']==0
        app.advance(305000); snapshots()
        pump(lambda:state_is('sensor.cleverbox_1_temperature','unavailable'))
        assert state_is('sensor.cleverbox_2_temperature','unavailable')
        app.sim('inject',frame=REPORT); app.sim('inject',frame=REPORT_B); app.advance(10); snapshots()
        pump(lambda:state_is('binary_sensor.cleverbox_1_observation_fresh','on'))
        print('HA rapid actions, busy rejection, two radiators, RF timeout, stale reports and recovery: passed', flush=True)
        # Missing snapshots expire in real HA wall time despite online availability.
        # --test-clock freezes publication while HA's native expire_after runs.
        until = time.monotonic()+18
        while time.monotonic()<until:
            observer.loop(timeout=.05)
            if state_is('sensor.cleverbox_1_snapshot','unavailable'): break
            time.sleep(.1)
        assert state_is('sensor.cleverbox_1_snapshot','unavailable')
        assert state_is('sensor.cleverbox_1_temperature','unavailable')
        assert state_is('button.cleverbox_1_apply','unavailable')
        snapshots(); pump(lambda:state_is('binary_sensor.cleverbox_1_observation_fresh','on'))
        # HA restart preserves MQTT entry but never restores/replays an action.
        tx = app.http()['simulation_tx']; original_session = session()
        ha.stop(); ha = HomeAssistant(config,port); snapshots()
        assert session()==original_session and app.http()['simulation_tx']==tx
        apply(2,17,'Frost'); pump(lambda:phase(2,'matched') and app.http()['radiators'][1]['observed']['mode']==2)
        print('HA missing-snapshot expiry, recovery and process restart without replay: passed', flush=True)
        # Admit RF then remove the broker. Delivery proceeds during network loss.
        snapshots(); app.sim('scenario',name='delayed'); apply(2,18,'Comfort')
        pump(lambda:app.http()['radiators'][1]['command']['pending'],step=100)
        broker.terminate(); broker.wait(timeout=5)
        app.advance(6000); assert phase(2,'matched')
        tx = app.http()['simulation_tx']; broker = start_broker(); observer.reconnect()
        pump(lambda:app.http('/api/mqtt')['connected'] and session()!=original_session,timeout=20)
        changed_session = session(); snapshots()
        assert app.http()['simulation_tx']==tx
        pump(lambda:state_is('sensor.cleverbox_2_last_delivery','matched'))
        result = ha.states()['sensor.cleverbox_2_last_delivery']['attributes']['result']
        assert result['session']==original_session
        apply(1,20,'Off'); pump(lambda:phase(1,'matched') and app.http()['observed']['mode']==1)
        # Restart actual bridge process with a pending command. Boot cannot resume it.
        snapshots(); app.sim('scenario',name='silence'); apply(2,19,'Eco')
        pump(lambda:app.http()['radiators'][1]['command']['pending'])
        old_command = [p for t,p,r in messages if t.endswith('/2/set')][-1]
        app.stop(); app = App(storage,real_mqtt=True)
        app.sim('network_event',name='started'); app.sim('network_event',name='got_ip')
        pump(lambda:app.http('/api/mqtt')['connected'])
        assert session()!=changed_session and app.http()['simulation_tx']==[]
        assert not app.http('/api/time')['usable'] and all(r['command']['phase']=='idle' for r in app.http()['radiators'])
        snapshots(); assert state_is('sensor.cleverbox_1_temperature','unavailable')
        observer.publish(BASE+'/radiators/2/set',json.dumps(old_command))
        pump(lambda:admitted(2,'session_mismatch'))
        assert app.http()['simulation_tx']==[]
        app.http('/api/time',dict(action='manual',unix_seconds=1767268800)); snapshots()
        apply(2,19,'Eco'); pump(lambda:phase(2,'matched'))
        sets = [(p,r) for t,p,r in messages if t.endswith('/set')]
        assert sets and not any(retain for _,retain in sets)
        assert all(set(p)=={'session','command_token','mode','centi_c'} for p,_ in sets)
        print('HA broker/bridge restarts, new sessions, RF continuity and no reconnect replay: passed',flush=True)
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
