#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Loopback HTTP/socket adapter only. Protocol/application logic stays in C/C++."""
import argparse
import ctypes
import json
import os
from pathlib import Path
import time
from http.server import HTTPServer, BaseHTTPRequestHandler

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=8080)
    parser.add_argument('--storage', type=Path, default=ROOT / 'local/simulation-pairing.bin')
    parser.add_argument('--test-clock', action='store_true', help='Freeze clock; enable test advance/inject API')
    args = parser.parse_args()
    args.storage.parent.mkdir(parents=True, exist_ok=True)
    os.environ['CLEVERBOX_SIM_STORAGE'] = str(args.storage.resolve())
    lib = ctypes.CDLL(str(ROOT / 'build/host/libcleverbox.so'))
    lib.host_request.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p]
    lib.host_response.restype = lib.host_response_type.restype = ctypes.c_char_p
    lib.simulation_scenario.argtypes = lib.simulation_inject.argtypes = [ctypes.c_char_p]
    lib.simulation_advance.argtypes = [ctypes.c_int]
    lib.host_init()
    last = time.monotonic()

    def tick():
        nonlocal last
        if not args.test_clock:
            now = time.monotonic()
            ms = int((now - last) * 1000)
            lib.simulation_advance(ms)
            last += ms / 1000

    class Handler(BaseHTTPRequestHandler):
        def reply(self, code, data, kind='text/plain'):
            if isinstance(data, str):
                data = data.encode()
            self.send_response(code)
            self.send_header('Content-Type', kind)
            self.send_header('Cache-Control', 'no-store')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            tick()
            if self.path == '/':
                self.reply(200, (ROOT / 'firmware/main/index.html').read_bytes(), 'text/html; charset=utf-8')
            else:
                self.production(0, b'')

        def do_POST(self):
            tick()
            try:
                length = int(self.headers.get('Content-Length', '0'))
            except ValueError:
                self.reply(400, 'Invalid body length')
                return
            if not 0 < length <= 1023 or self.headers.get('Transfer-Encoding'):
                self.reply(400, 'Invalid body length')
                return
            body = self.rfile.read(length)
            if len(body) != length or b'\0' in body:
                self.reply(400, 'Invalid body')
                return
            if self.path == '/api/simulation':
                self.simulation(body)
            else:
                self.production(1, body)

        def production(self, method, body):
            code = lib.host_request(method, self.path.encode(), body, self.headers.get('Content-Type', '').encode())
            self.reply(code, lib.host_response(), lib.host_response_type().decode())

        def simulation(self, body):
            if self.headers.get('Content-Type') != 'application/json':
                self.reply(400, 'Use application/json')
                return
            try:
                data = json.loads(body)
                action = data.get('action')
                if action == 'fixture':
                    lib.simulation_fixture()
                elif action == 'scenario' and isinstance(data.get('name'), str):
                    if not lib.simulation_scenario(data['name'].encode()):
                        raise ValueError()
                elif args.test_clock and action == 'advance' and type(data.get('ms')) is int and 0 <= data['ms'] <= 600000:
                    lib.simulation_advance(data['ms'])
                elif args.test_clock and action == 'inject' and isinstance(data.get('frame'), str):
                    if not lib.simulation_inject(data['frame'].encode()):
                        raise ValueError()
                else:
                    raise ValueError()
            except (ValueError, AttributeError):
                self.reply(400, 'Invalid simulation action')
                return
            self.reply(202, '{"accepted":true}', 'application/json')

        def log_message(self, *unused):
            pass

    # Single owner of C worker/status/queue; HTTP and clock steps never race.
    server = HTTPServer(('127.0.0.1', args.port), Handler)
    server.timeout = 0.02
    print(f'SIMULATION ONLY: http://127.0.0.1:{server.server_port}', flush=True)
    try:
        while True:
            server.handle_request()
            tick()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == '__main__':
    main()
