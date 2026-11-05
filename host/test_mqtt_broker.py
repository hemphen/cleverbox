#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Real Mosquitto 2.x broker with the host simulation (paho-mqtt==2.1.0)."""
import json
import os
from pathlib import Path
import pwd
import shutil
import socket
import subprocess
import tempfile
import time
import paho.mqtt.client as mqtt
from test_http import App, seed_pairing

BASE = 'cleverbox/v1/020000000001'


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


class Broker:
    def __init__(self, directory):
        self.port = free_port()
        self.conf = Path(directory) / 'mosquitto.conf'
        self.conf.write_text(f'listener {self.port} 127.0.0.1\nallow_anonymous true\n'
                             f'persistence false\nuser {pwd.getpwuid(os.geteuid()).pw_name}\n')
        self.log = open(Path(directory) / 'broker.log', 'wb')
        self.start()

    def start(self):
        self.process = subprocess.Popen([shutil.which('mosquitto'), '-c', str(self.conf)], stdout=self.log, stderr=self.log)
        until = time.monotonic() + 5
        while True:
            try:
                socket.create_connection(('127.0.0.1', self.port), timeout=.1).close()
                return
            except OSError:
                assert time.monotonic() < until
                time.sleep(.02)

    def stop(self):
        self.process.terminate()
        self.process.wait(timeout=5)


def run(directory):
    storage = Path(directory) / 'bridge'
    seed_pairing(storage)
    broker = Broker(directory)
    app = App(storage, real_mqtt=True)
    messages = []
    observer = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id='cleverbox-test-observer')
    observer.on_connect = lambda c, u, f, r, p: c.subscribe(BASE + '/#')
    observer.on_message = lambda c, u, m: messages.append(
        (m.topic, json.loads(m.payload) if m.payload.startswith(b'{') else m.payload.decode(), m.retain))
    observer.connect('127.0.0.1', broker.port)

    def pump(predicate, timeout=10):
        until = time.monotonic() + timeout
        while time.monotonic() < until:
            app.advance(100)
            observer.loop(timeout=.02)
            if predicate():
                return
        raise AssertionError(('timed out', messages[-6:]))

    def result():
        return [p for t, p, r in messages if t == BASE + '/radiators/1/result'][-1]

    try:
        app.http('/api/mqtt', {'action': 'configure', 'host': '127.0.0.1', 'port': broker.port,
                               'username': '', 'password': '', 'enabled': True})
        pump(lambda: app.http('/api/mqtt')['connected'])
        pump(lambda: any(t == BASE + '/availability' and p == 'online' for t, p, r in messages))
        app.http('/api/time', {'action': 'manual', 'unix_seconds': 1767225600})
        observer.publish(BASE + '/radiators/1/set', json.dumps({'mode': 0, 'centi_c': 2150, 'request_id': 1}))
        pump(lambda: any(t.endswith('/result') for t, p, r in messages))
        assert result() == {'request_id': 1, 'accepted': True, 'reason': 'accepted'}
        pump(lambda: app.http()['radiators'][0]['command']['phase'] == 'matched')
        pump(lambda: any(t.endswith('/1/state') and p['command']['phase'] == 'matched' for t, p, r in messages), timeout=15)
        # A retained command reaches a new subscription, such as at boot, with
        # the retain flag set. It is rejected and never sent to the radiator.
        app.http('/api/mqtt', {'action': 'disable'})
        pump(lambda: not app.http('/api/mqtt')['connected'])
        tx = app.http()['simulation_tx']
        observer.publish(BASE + '/radiators/1/set', json.dumps({'mode': 3, 'centi_c': 1800}), retain=True)
        app.http('/api/mqtt', {'action': 'enable'})
        pump(lambda: result()['reason'] == 'retained_command')
        observer.publish(BASE + '/radiators/1/set', b'', retain=True)
        assert app.advance(2000)['simulation_tx'] == tx
        # Broker restart: the client reconnects by itself and replays nothing.
        broker.stop()
        pump(lambda: not app.http('/api/mqtt')['connected'])
        broker.start()
        observer.reconnect()
        pump(lambda: app.http('/api/mqtt')['connected'], timeout=20)
        assert app.http()['simulation_tx'] == tx
        # A bridge that disappears leaves its retained will.
        messages.clear()
        app.process.kill(); app.process.wait(); app.process.stdout.close()
        until = time.monotonic() + 60
        while not any(t == BASE + '/availability' and p == 'offline' for t, p, r in messages):
            assert time.monotonic() < until, messages[-4:]
            observer.loop(timeout=.1)
    finally:
        if app.process.poll() is None:
            app.stop()
        observer.disconnect()
        broker.stop()


if __name__ == '__main__':
    assert shutil.which('mosquitto'), 'Install Mosquitto 2.x first'
    with tempfile.TemporaryDirectory() as directory:
        run(directory)
    print('Real broker: connect, availability, command and readback, retained rejection, reconnect and will: passed')
