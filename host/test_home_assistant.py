#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Real Home Assistant Core 2026.10.4 and Mosquitto with the host simulation.

Loads the shipped YAML package, configures HA's MQTT integration, presses
Apply and reads real entity states. Only the radio and Wi-Fi are simulated.
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
from test_http import seed_pairing, App, ROOT, report_from


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
    with open(directory / 'check-config.log', 'w') as check_log:
        subprocess.run([sys.executable, '-m', 'homeassistant', '--script', 'check_config', '--config', str(config)],
                       stdout=check_log, stderr=check_log, check=True, timeout=90)
    port = free_port()
    conf = directory / 'mosquitto.conf'
    conf.write_text(f'listener {port} 127.0.0.1\nallow_anonymous true\npersistence false\nuser {pwd.getpwuid(os.geteuid()).pw_name}\n')
    broker_log = open(directory / 'broker.log', 'wb')
    broker = subprocess.Popen([shutil.which('mosquitto'), '-c', str(conf)], stdout=broker_log, stderr=broker_log)
    time.sleep(.5)
    storage = directory / 'bridge'; seed_pairing(storage)
    app = App(storage, real_mqtt=True); ha = None

    def pump(predicate, timeout=20):
        until = time.monotonic() + timeout
        while time.monotonic() < until:
            app.advance(100)
            if predicate(): return
            time.sleep(.02)
        raise AssertionError(('condition timed out', ha.states() if ha else None))
    def state(entity):
        return ha.states().get(entity, {}).get('state')
    keys = ('temperature', 'observed_target', 'observed_mode', 'observation_fresh', 'report_age', 'last_command', 'last_admission')
    def discovered():  # Discovery entities only, not the package's own.
        return {e['unique_id'] for e in ha.rpc(op='registries')['entities']
                if e['unique_id'].startswith('cleverbox_020000000001_') and e['unique_id'].endswith(keys)}
    def apply(i, target, mode):
        ha.call('input_number', 'set_value', entity_id=f'input_number.cleverbox_{i}_draft_target', value=target)
        ha.call('input_select', 'select_option', entity_id=f'input_select.cleverbox_{i}_draft_mode', option=mode)
        ha.call('button', 'press', entity_id=f'button.cleverbox_{i}_apply')
    try:
        app.sim('device', index=1)
        app.http('/api/pairing', dict(action='start', source='F1010203', destination='F1100001'), 202); app.advance(2000)
        app.sim('inject', frame=report_from('F1445566', 'D0123456')); app.advance(10)
        app.http('/api/radiators', dict(action='monitor', radiator='F1445566', name='Hall'))
        app.http('/api/mqtt', dict(action='configure', host='127.0.0.1', port=port, username='', password='', enabled=True))
        app.http('/api/mqtt', dict(action='discovery_enable'))
        pump(lambda: app.http('/api/mqtt')['connected'])
        ha = HomeAssistant(config, port)
        prefix = 'cleverbox_020000000001_'
        pump(lambda: len(discovered()) == 7 + 7 + 5)
        assert {prefix + '3_' + k for k in keys[:5]} <= discovered()
        pump(lambda: state('sensor.cleverbox_020000000001_3_temperature') == '20.0')
        assert state('sensor.cleverbox_020000000001_1_temperature') == 'unavailable'
        app.http('/api/radiators', dict(action='rename', id=1, name='Living room'))
        pump(lambda: any(d['name'] == 'Living room' for d in ha.rpc(op='registries')['devices']))
        print('HA package check, discovery of paired and monitored radiators, rename: passed', flush=True)
        # Apply needs usable time; without it the button is unavailable.
        assert state('button.cleverbox_1_apply') == 'unavailable'
        app.http('/api/time', dict(action='manual', unix_seconds=1767225600))
        pump(lambda: state('button.cleverbox_1_apply') != 'unavailable')
        tx = len(app.http()['simulation_tx'])
        apply(1, 22, 'Comfort')
        pump(lambda: state('sensor.cleverbox_020000000001_1_last_admission') == 'accepted')
        pump(lambda: app.http()['radiators'][0]['command']['phase'] == 'matched')
        pump(lambda: state('sensor.cleverbox_020000000001_1_observed_target') == '22.0' and
             state('sensor.cleverbox_020000000001_1_last_command') == 'matched')
        assert state('sensor.cleverbox_020000000001_1_observed_mode') == 'Comfort'
        apply(2, 16, 'Eco')
        pump(lambda: state('sensor.cleverbox_020000000001_2_observed_mode') == 'Eco')
        assert len(app.http()['simulation_tx']) > tx
        print('HA Apply, admission and command results, observed readback: passed', flush=True)
        # Radio failure: readings and Apply become unavailable; retry restores them.
        app.sim('radio_fault', stage='receive', code=-2)
        app.sim('inject', frame=report_from('F1445566', 'D0123456')); app.advance(10)
        pump(lambda: state('sensor.cleverbox_020000000001_1_temperature') == 'unavailable' and
             state('binary_sensor.cleverbox_ready') == 'off')
        app.sim('radio_fault', stage='receive', code=0)
        app.http('/api/radio', dict(action='retry'), 202)
        pump(lambda: state('sensor.cleverbox_020000000001_1_temperature') == '20.0')
        # Snapshots stop: entities expire after 15 s of wall time.
        until = time.monotonic() + 20
        while time.monotonic() < until and state('sensor.cleverbox_020000000001_1_temperature') != 'unavailable':
            time.sleep(.2)
        assert state('sensor.cleverbox_020000000001_1_temperature') == 'unavailable'
        pump(lambda: state('sensor.cleverbox_020000000001_1_temperature') == '20.0')
        print('HA radio failure, retry and snapshot expiry: passed', flush=True)
        # Removal and disabling discovery remove the devices.
        app.http('/api/radiators', dict(action='remove', id=3))
        pump(lambda: len(discovered()) == 14)
        app.http('/api/mqtt', dict(action='discovery_disable'))
        pump(lambda: not discovered())
        ha.stop(); ha = HomeAssistant(config, port)
        pump(lambda: not discovered())
        print('HA removal, discovery disable and HA restart: passed', flush=True)
    finally:
        if ha: ha.stop()
        app.stop()
        broker.terminate(); broker.wait(timeout=5); broker_log.close()


if __name__ == '__main__':
    assert shutil.which('mosquitto'), 'Install Mosquitto 2.x first'
    # Keep logs for reproducibility when explicitly requested; otherwise temporary.
    if path := os.environ.get('CLEVERBOX_HA_TEST_DIR'):
        Path(path).mkdir(parents=True,exist_ok=True); run(path)
    else:
        with tempfile.TemporaryDirectory() as directory: run(directory)
    print('Actual Home Assistant Core 2026.10.4 / Mosquitto integration: passed')
