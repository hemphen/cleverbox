#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Serve the host simulation over HTTP. Application logic stays in C/C++."""
import argparse
import ctypes
import json
import os
from pathlib import Path
import select
import time
from http.server import HTTPServer, BaseHTTPRequestHandler

ROOT = Path(__file__).resolve().parents[1]


def load(path):
    lib = ctypes.CDLL(path)
    lib.host_request.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p]
    lib.host_response.restype = lib.host_response_type.restype = lib.host_mqtt_pop.restype = ctypes.c_char_p
    lib.simulation_scenario.argtypes = lib.simulation_inject.argtypes = lib.host_network_event.argtypes = [ctypes.c_char_p]
    lib.simulation_radio_fault.argtypes = [ctypes.c_char_p, ctypes.c_int]
    lib.host_time_sync.argtypes = [ctypes.c_int64]
    lib.mqtt_bridge_message.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_bool]
    return lib


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=8080)
    parser.add_argument('--storage', type=Path, default=ROOT / 'local/simulation')
    parser.add_argument('--real-mqtt', action='store_true', help='Connect to a real broker (needs paho-mqtt==2.1.0)')
    parser.add_argument('--test-clock', action='store_true', help='Freeze the clock; enable test actions')
    args = parser.parse_args()
    args.storage.parent.mkdir(parents=True, exist_ok=True)
    os.environ['CLEVERBOX_SIM_STORAGE'] = str(args.storage.resolve())
    lib = load(os.environ.get('CLEVERBOX_SIM_LIBRARY', str(ROOT / 'build/host/libcleverbox.so')))
    lib.host_init()
    adapter = None
    if args.real_mqtt:
        from mqtt_client import BrokerClient
        adapter = BrokerClient(lib)
    last = time.monotonic()

    def tick():
        nonlocal last
        if adapter:
            adapter.poll()
        if not args.test_clock:
            ms = int((time.monotonic() - last) * 1000)
            lib.simulation_advance(ms)
            last += ms / 1000

    def simulate(data):
        action = data.get('action')
        if action == 'device':
            return lib.simulation_device(int(data['index']))
        if action == 'scenario':
            return lib.simulation_scenario(data['name'].encode())
        if not args.test_clock:
            return 0
        if action == 'advance' and 0 <= int(data['ms']) <= 600000:
            lib.simulation_advance(int(data['ms']))
            return 1
        if action == 'inject':
            return lib.simulation_inject(data['frame'].encode())
        if action == 'radio_fault':
            return lib.simulation_radio_fault(data['stage'].encode(), int(data['code']))
        if action == 'time_sync':
            return lib.host_time_sync(int(data['unix_seconds']))
        if action == 'network_event':
            return lib.host_network_event(data['name'].encode())
        if action == 'button':
            lib.host_network_button(int(bool(data['low'])))
            return 1
        if action == 'mqtt_connection':
            return lib.host_mqtt_connection(int(bool(data['connected'])))
        if action == 'mqtt_message':
            topic, payload = data['topic'].encode(), data['payload'].encode()
            lib.mqtt_bridge_message(topic, payload, len(payload), bool(data.get('retained', False)))
            return 1
        return 0

    class Handler(BaseHTTPRequestHandler):
        timeout = 0.5

        def reply(self, code, data, kind='text/plain'):
            data = data.encode() if isinstance(data, str) else data
            self.send_response(code)
            self.send_header('Content-Type', kind)
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            tick()
            if self.path == '/':
                self.reply(200, (ROOT / 'firmware/main/index.html').read_bytes(), 'text/html; charset=utf-8')
            elif args.test_clock and self.path == '/api/simulation/mqtt':
                rows = []
                while row := lib.host_mqtt_pop():
                    rows.append(json.loads(row))
                self.reply(200, json.dumps(rows), 'application/json')
            else:
                self.production(0, b'')

        def do_POST(self):
            tick()
            body = self.rfile.read(int(self.headers.get('Content-Length', '0')))
            if self.path != '/api/simulation':
                self.production(1, body)
                return
            try:
                ok = simulate(json.loads(body))
            except (ValueError, KeyError, TypeError, AttributeError):
                ok = 0
            self.reply(202 if ok else 400, '{}' if ok else '{"error":"invalid_simulation_action"}', 'application/json')

        def production(self, method, body):
            code = lib.host_request(method, self.path.encode(), body, self.headers.get('Content-Type', '').encode())
            self.reply(code, lib.host_response(), lib.host_response_type().decode())

        def log_message(self, *unused):
            pass

    # One thread owns the C application; HTTP and clock steps never race.
    server = HTTPServer(('127.0.0.1', args.port), Handler)
    print(f'SIMULATION ONLY: http://127.0.0.1:{server.server_port}', flush=True)
    try:
        while True:
            if select.select([server], [], [], 0.02)[0]:
                server.handle_request()
            tick()
    except KeyboardInterrupt:
        pass
    finally:
        if adapter:
            adapter.stop()
        server.server_close()


if __name__ == '__main__':
    main()
