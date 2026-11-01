#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Production time/HTTP/admission/worker integration; fake monotonic/SNTP I/O only."""
from pathlib import Path
import tempfile
from test_http import App, STATE, REPORT

TZ = 'CET-1CEST,M3.5.0/2,M10.5.0/3'


def states(app):
    return [bytes.fromhex(f) for f in app.http()['simulation_tx'] if len(f) == 118]


def clock(frame):
    return frame[52] * 3600 + frame[53] * 60 + frame[54]


def run(directory):
    storage = Path(directory) / 'time.bin'
    storage.write_bytes(bytes.fromhex('01F1010203F1040506'))
    app = App(storage)
    try:
        s = app.http('/api/time')
        assert not s['usable'] and s['source'] == 'unsynchronized'
        assert not s['synchronized_this_boot'] and s['sync_age_ms'] is None and 'utc' not in s
        assert s['timezone'] == 'UTC0'
        app.http('/api/controller-state', {'action': 'import', 'state': STATE}, 202)
        app.advance(1)
        error = app.http('/api/command', {'mode': 0, 'centi_c': 2050}, 409)
        assert 'Device time unavailable' in error and 'Wi-Fi' in error and 'UTC' in error
        assert app.http()['simulation_tx'] == [] and not app.http()['command']['pending']
        for value in (None, '', -1, 86400, 1.5, True):
            app.http('/api/command', {'mode': 0, 'centi_c': 2050, 'clock_seconds': value}, 400)
        # Explicit legacy/test override takes precedence even on an unset boot.
        app.http('/api/command', {'mode': 0, 'centi_c': 2050, 'clock_seconds': 43200}, 202)
        app.advance(1000)
        assert clock(states(app)[-1]) == 43200 and app.http()['command']['phase'] == 'matched'
        assert not app.http('/api/time')['usable']  # Override does not establish device time.
        tx = app.http()['simulation_tx']
        for data in ({}, {'action': 'manual'}, {'action': 'manual', 'unix_seconds': 0},
                     {'action': 'manual', 'unix_seconds': 1767225600.5},
                     {'action': 'manual', 'unix_seconds': '1767225600'},
                     {'action': 'manual', 'unix_seconds': True},
                     {'action': 'manual', 'unix_seconds': 4102444800},
                     {'action': 'timezone', 'timezone': 'Europe/Stockholm'},
                     {'action': 'timezone', 'timezone': 'CET-1CEST'},
                     {'action': 'timezone', 'timezone': 'UTC0', 'extra': 1},
                     {'action': 'timezone', 'timezone': 'UTC0\x00ignored'}):
            app.http('/api/time', data, 400)
        app.http('/api/time', raw=b'{"action":"timezone","timezone":"UTC0"}\x00', code=400)
        app.http('/api/time', {'action': 'timezone', 'timezone': 'UTC0'}, kind='text/plain', code=400)
        app.http('/api/time', {'action': 'timezone', 'timezone': TZ})
        app.http('/api/time', {'action': 'manual', 'unix_seconds': 1767218399}) # 21:59:59 UTC -> 22:59:59 CET
        s = app.http('/api/time')
        assert s['source'] == 'manual' and s['usable'] and not s['synchronized_this_boot']
        assert s['local'].endswith('22:59:59+0100')
        assert app.http()['simulation_tx'] == tx
        config = Path(str(storage) + '.timezone').read_bytes()
        assert config == b'\x01' + TZ.encode() + b'\x00'
        # Delay/retry through midnight, ignoring subsequent timezone/wall corrections.
        app.http('/api/time', {'action': 'manual', 'unix_seconds': 1767221999}) # 23:59:59 CET
        app.sim('scenario', name='ack_only')
        app.http('/api/command', {'mode': 3, 'centi_c': 1550}, 202)
        app.advance(1)  # poll
        app.http('/api/time', {'action': 'manual', 'unix_seconds': 1893456000}) # forward correction
        app.http('/api/time', {'action': 'timezone', 'timezone': 'UTC0'})
        app.advance(2499)  # first response at 300ms; receive-first TX follows in the same tick
        first = states(app)[-1]
        assert clock(first) == 86399 and first[37] == 3
        assert app.http()['command']['phase'] == 'waiting_readback'
        app.sim('scenario', name='silence')
        app.http('/api/time', {'action': 'manual', 'unix_seconds': 1577836800}) # backward correction
        app.sim('time_sync', unix_seconds=1924992000) # first network sync; forward again
        assert app.http('/api/time')['synchronized_this_boot']
        app.advance(7500)
        app.sim('inject', frame=REPORT) # mismatching report permits second state, not a blind retry
        app.advance(1)  # injected report and ready state TX share this receive-first iteration
        assert clock(states(app)[-1]) == 9, (clock(states(app)[-1]), app.http()['command'], [clock(f) for f in states(app)])
        assert app.http()['command']['attempts'] == 1  # Physical state TX is still outstanding.
        app.advance(1)  # Resolve TX before starting the readback deadline.
        assert app.http()['command']['attempts'] == 2
        before = app.http()['simulation_tx']
        app.http('/api/time', {'action': 'manual', 'unix_seconds': 1767225600})
        app.advance(149999)
        assert app.http()['command']['phase'] == 'waiting_readback'
        # Wall correction cannot affect the exact monotonic report deadline.
        app.sim('time_sync', unix_seconds=2051222400)
        assert app.http()['command']['phase'] == 'waiting_readback'
        app.advance(1)
        assert app.http()['command']['phase'] == 'timeout' and app.http()['simulation_tx'] == before
        # Time ages and observation ages advance monotonically despite wall corrections.
        age = app.http()['observed']['age_ms']
        app.sim('time_sync', unix_seconds=1767225600)
        assert app.http()['observed']['age_ms'] == age
        app.advance(1000)
        assert app.http()['observed']['age_ms'] == age + 1000
        assert app.http('/api/time')['sync_age_ms'] == 1000
        # Connect/reconnect events and sync never admit pairing/settings or persist time.
        import secrets
        app.http('/api/network', {'action': 'configure', 'ssid': secrets.token_hex(6), 'password': secrets.token_hex(8)}, 202)
        app.sim('network_event', name='started'); app.sim('network_event', name='got_ip'); app.advance(1)
        assert app.http('/api/time')['network_connected']
        app.sim('time_sync', unix_seconds=1774745999)
        app.http('/api/time', {'action': 'timezone', 'timezone': TZ})
        assert app.http('/api/time')['local'].endswith('01:59:59+0100')
        app.advance(1000)
        assert app.http('/api/time')['local'].endswith('03:00:00+0200')
        app.sim('network_event', name='disconnected'); app.advance(1000)
        s = app.http('/api/time')
        assert not s['network_connected'] and s['source'] == 'network' and s['usable']
        assert s['synchronized_this_boot'] and s['sync_age_ms'] == 2000
        app.advance(5000); app.sim('network_event', name='started'); app.sim('network_event', name='got_ip'); app.advance(1)
        app.sim('time_sync', unix_seconds=1792889999)
        assert app.http('/api/time')['sync_age_ms'] == 0
        assert app.http('/api/time')['local'].endswith('02:59:59+0200')
        app.advance(1000)
        assert app.http('/api/time')['local'].endswith('02:00:00+0100')
        assert app.http()['simulation_tx'] == before
        assert Path(str(storage) + '.timezone').read_bytes() == config
        app.http('/api/time', {'action': 'manual', 'unix_seconds': 1767268800})
        s = app.http('/api/time')
        assert s['source'] == 'manual' and s['synchronized_this_boot'] and s['sync_age_ms'] == 1000
        app.http('/api/command', {'mode': 2, 'centi_c': 700}, 202)
        assert app.http()['command']['pending']  # Reboot discards this normally admitted command.
    finally:
        app.stop()
    app = App(storage)
    try:
        s = app.http('/api/time')
        assert s['timezone'] == TZ and not s['usable'] and s['source'] == 'unsynchronized'
        assert not s['synchronized_this_boot'] and s['sync_age_ms'] is None
        app.advance(1000)
        assert app.http()['simulation_tx'] == [] and app.http()['command']['phase'] == 'idle'
        app.http('/api/command', {'mode': 0, 'centi_c': 2050}, 409)
        app.http('/api/time', {'action': 'manual', 'unix_seconds': 1767268800})
        app.sim('scenario', name='delayed')
        app.http('/api/command', {'mode': 0, 'centi_c': 2050}, 202)
        app.advance(2502)
        assert clock(states(app)[-1]) == 13 * 3600 + 2
        app.advance(2500)
        assert app.http()['command']['phase'] == 'matched'
    finally:
        app.stop()
    # Malformed configuration is surfaced; explicit save repairs only its own key.
    Path(str(storage) + '.timezone').write_bytes(b'\x02UTC0\x00')
    app = App(storage)
    try:
        s = app.http('/api/time')
        assert s['config_error'] and s['timezone'] == 'UTC0' and not s['usable']
        app.http('/api/time', {'action': 'timezone', 'timezone': TZ})
        assert not app.http('/api/time')['config_error'] and app.http()['simulation_tx'] == []
    finally:
        app.stop()


def network_deadlines(directory):
    import secrets
    app = App(Path(directory) / 'network-deadlines.bin')
    try:
        app.http('/api/network', {'action': 'configure', 'ssid': secrets.token_hex(6), 'password': secrets.token_hex(8)}, 202)
        app.sim('network_event', name='started')
        app.sim('time_sync', unix_seconds=2051222400)
        app.advance(14999)
        assert app.http('/api/network')['state'] == 'connecting'
        app.sim('time_sync', unix_seconds=1577836800)
        assert app.http('/api/network')['state'] == 'connecting'
        app.advance(1)
        assert app.http('/api/network')['state'] == 'disconnected'
        app.advance(4999)
        assert app.http('/api/network')['attempts'] == 1
        app.sim('time_sync', unix_seconds=2051222400)
        app.advance(1)
        assert app.http('/api/network')['attempts'] == 2
        app.sim('network_event', name='started'); app.sim('network_event', name='got_ip')
        app.advance(59999)
        assert app.http('/api/network')['recovery_ap']
        app.sim('time_sync', unix_seconds=1577836800)
        app.advance(1)
        assert not app.http('/api/network')['recovery_ap']
        assert app.http()['simulation_tx'] == []
    finally:
        app.stop()


if __name__ == '__main__':
    with tempfile.TemporaryDirectory() as directory:
        run(directory)
        network_deadlines(directory)
    print('HTTP device time: boot/manual/sync/offline/reconnect, corrections, deadlines/retries, DST, persistence and no automatic RF: passed')
