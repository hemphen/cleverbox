#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Two-device HTTP acceptance through the production handlers and receive worker."""
from pathlib import Path
import tempfile
from test_http import App, STATE, REPORT, COMFORT, changed_frame

# Independently assembled and CRC-sealed Python vectors; no production or
# simulator builder supplies these expected packets. All identities are invented.
REPORT_B = '23F1040507C6F1010203DF203000404B02A88A02BA00E100641919DA000A000A0A47CC18D07D'
POLL_B = '0DF1010203C6F10405071900F1DB278A'
BIND_B = '0DF1010203C6F10405070100FBDB7B8F'


def selected(app, identity):
    return app.http('/api/radio?id=' + str(identity))


def pair(app, index):
    app.sim('device', index=index)
    return app.http('/api/pairing', {'action': 'start', 'source': 'F1010203', 'destination': 'F1100001'}, 202)['id']


def mutate(app, identity, action, **kw):
    app.http('/api/radiators', {'id': identity, 'action': action, **kw}, 202)
    s = app.advance(1)
    assert not s['mutation_pending'] and not s['mutation_error'], s


def registry_blob(count, next_id=None):
    """Generated capacity fixtures: no simulator limit or special device paths."""
    data = bytearray([2]) + count.to_bytes(2, 'little') + (next_id or count + 1).to_bytes(4, 'little')
    for i in range(1, count + 1):
        name = ('Generated ' + str(i)).encode().ljust(33, b'\0')
        radiator = bytes([0xf1, 0x20, i >> 8, i & 255])
        data += i.to_bytes(4, 'little') + name + bytes.fromhex('F1010203') + radiator
    return data


def two_devices(directory):
    path = Path(directory) / 'two.bin'
    disk = Path(str(path) + '.registry')
    app = App(path)
    try:
        assert app.advance(1000)['simulation_tx'] == []
        a = pair(app, 0)
        assert a > 0
        app.http('/api/command', {'id': a, 'mode': 0, 'centi_c': 2050, 'clock_seconds': 0}, 409)
        app.http('/api/pairing', {'action': 'start', 'source': 'F1010203', 'destination': 'F1100001'}, 409)
        app.http('/api/pairing', {'id': a + 1, 'action': 'cancel'}, 409)
        assert app.advance(2000)['pairing']['saved']
        mutate(app, a, 'rename', name='Living room')
        assert 'template_available' not in selected(app,a)['command']
        b = pair(app, 1)
        assert b != a
        app.http('/api/command', {'id': a, 'mode': 0, 'centi_c': 2050, 'clock_seconds': 0}, 409)
        s = app.advance(2000)
        assert s['inclusion']['saved'] and s['simulation_tx'][-1] == BIND_B
        mutate(app, b, 'rename', name='Bedroom')
        assert 'template_available' not in selected(app,b)['command']
        assert len(app.http('/api/radiators')['radiators']) == 2
        assert selected(app, a)['name'] == 'Living room' and selected(app, b)['name'] == 'Bedroom'
        saved = disk.read_bytes()
        # Mutations with no identity are ambiguous. Malformed/unknown identities
        # never degrade to singleton/default/another client's selection.
        app.http('/api/command', {'mode': 0, 'centi_c': 2050, 'clock_seconds': 0}, 409)
        for endpoint, action in [('/api/radiators', 'remove'), ('/api/radiators', 'rename')]:
            app.http(endpoint, {'action': action, 'name': 'bad'}, 409)
        for identity in (None, 0, -1, 1.5, '1', True):
            app.http('/api/radiators', {'id': identity, 'action': 'remove'}, 400)
        for query in ('id=0', 'id=-1', 'id=1x', 'id=4294967296', 'other=1', 'id=1&id=2'):
            app.http('/api/radio?' + query, code=400)
        app.http('/api/radio?id=999', code=404)
        app.http('/api/radiators', {'id': a, 'action': 'rename', 'name': 'x' * 33}, 400)
        app.http('/api/radiators', {'id': a, 'action': 'rename', 'name': 'line\nbreak'}, 400)
        app.http('/api/command', {'id': a, 'mode': 0, 'centi_c': 2050, 'clock_seconds': 0, 'state': STATE}, 400)
        # A stays silent while B finishes; both can be admitted before worker dequeue.
        app.sim('scenario', name='silence_a')
        app.command(id=a)
        app.http('/api/radiators', {'id': b, 'action': 'select'})  # Another browser.
        app.command(1550, 3, id=b)
        assert app.http()['active_commands'] == 2
        app.http('/api/command', {'id': a, 'mode': 0, 'centi_c': 2050, 'clock_seconds': 0}, 409)
        app.http('/api/radiators', {'id': a, 'action': 'remove'}, 409)
        app.http('/api/pairing', {'action': 'start', 'source': 'F1010203', 'destination': 'F1100001'}, 409)
        app.advance(1000)
        assert selected(app, b)['command']['phase'] == 'matched'
        assert selected(app, a)['command']['phase'] == 'waiting_report'
        assert app.http()['active_commands'] == 1
        assert app.advance(149000)['active_commands'] == 1
        assert selected(app, a)['command']['phase'] == 'waiting_report'
        app.advance(2)  # Reply deadline starts at physical completion.
        assert selected(app, a)['command']['phase'] == 'timeout'
        assert selected(app, b)['command']['phase'] == 'matched'
        app.sim('scenario', name='normal')
        app.command(id=a)
        assert app.advance(1000)['simulation_tx'][-1] == COMFORT
        assert selected(app, a)['command']['phase'] == 'matched'
        assert selected(app, a)['observed']['mode'] == 0
        assert selected(app, b)['observed']['mode'] == 3
        # Interleaved reports drive separate state transmissions and retry bounds.
        app.sim('scenario', name='silence')
        app.command(id=a)
        app.advance(2)  # Resolve the outstanding physical TX before injecting RX.
        app.command(1550, 3, id=b)
        app.advance(2)  # Resolve the outstanding physical TX before injecting RX.
        app.sim('inject', frame=REPORT_B)
        app.advance(3)
        app.sim('inject', frame=REPORT)
        app.advance(3)
        assert selected(app, a)['command']['attempts'] == 1
        assert selected(app, b)['command']['attempts'] == 1
        app.sim('inject', frame=REPORT_B)
        app.advance(2)
        assert selected(app, b)['command']['attempts'] == 2
        assert selected(app, a)['command']['attempts'] == 1
        app.advance(150001)
        assert selected(app, a)['command']['phase'] == 'timeout'
        assert selected(app, b)['command']['phase'] == 'timeout'
        # Matching late report reconciles only the attempted A command, without TX.
        prior_tx = app.http()['simulation_tx']
        matching = '23F1040506C6F1010203DF203000404B02A88A02B100E100641919DA000A000A0A47C783C11D'
        app.sim('inject', frame=matching)
        app.advance(1)
        assert selected(app, a)['command']['phase'] == 'matched'
        assert selected(app, b)['command']['phase'] == 'timeout'
        assert app.http()['simulation_tx'] == prior_tx
        # Interleaved valid frames, wrong address pairs, corrupt frames and
        # distinct opaque controller bytes cannot contaminate another record.
        app.sim('scenario', name='silence')
        app.command(id=a)
        app.advance(2)  # Resolve the outstanding physical TX before injecting RX.
        app.sim('inject', frame=REPORT_B)
        app.advance(1)
        assert selected(app, a)['command']['phase'] == 'waiting_report'
        assert selected(app, b)['observed']['target_c'] == 21
        for frame in (changed_frame(REPORT, 9), changed_frame(REPORT, 4), REPORT[:-2] + '00',
                      changed_frame(STATE, 4), changed_frame(STATE, 9)):
            app.sim('inject', frame=frame)
            app.advance(1)
        assert selected(app, a)['command']['phase'] == 'waiting_report'
        assert disk.read_bytes() == saved  # Passive reception never writes flash.
        app.http('/api/radiators', {'id': b, 'action': 'select'})
        app.sim('inject', frame=REPORT)
        app.advance(2)
        assert app.http()['simulation_tx'][-1] == COMFORT
        assert selected(app, a)['command']['phase'] == 'waiting_readback'
        # A pending generated command ignores non-report RF traffic.
        app.sim('inject', frame=changed_frame(STATE, 49))
        app.advance(1)
        assert disk.read_bytes() == saved
        # Finish the active exchange with the independent matching report vector.
        matching = '23F1040506C6F1010203DF203000404B02A88A02B100E100641919DA000A000A0A47C783C11D'
        app.sim('inject', frame=matching)
        app.advance(1)
        assert selected(app, a)['command']['phase'] == 'matched'
        # Duplicate is detected from inclusion reply before transmitting bind.
        app.sim('scenario', name='normal')
        duplicate_id = pair(app, 1)
        app.advance(2000)
        s = app.http()
        assert s['inclusion']['duplicate'] and s['inclusion']['error']
        assert len(s['radiators']) == 2 and disk.read_bytes() == saved
        assert s['simulation_tx'][-1].startswith('0FF1010203C6F1100001')
        assert not s['exchange_id']
        # Removed records cannot be targeted by stale browser selections.
        assert duplicate_id not in (a, b)
    finally:
        app.stop()
    app = App(path)
    try:
        s = app.advance(600000)
        assert s['simulation_tx'] == [] and len(s['radiators']) == 2
        for identity, name in ((a, 'Living room'), (b, 'Bedroom')):
            d = selected(app, identity)
            assert d['name'] == name and d['command']['frame_source'] == 'generated-touch-e3'
            assert d['command']['phase'] == 'idle' and 'observed' not in d
        # A storage commit failure must preserve old RAM/disk; explicit retry works.
        blocker = Path(str(disk) + '.tmp'); blocker.mkdir()
        app.http('/api/radiators', {'id': b, 'action': 'rename', 'name': 'Failed name'}, 202)
        assert app.advance(1)['mutation_error']
        assert selected(app, b)['name'] == 'Bedroom' and disk.read_bytes() == saved
        blocker.rmdir()
        mutate(app, b, 'rename', name='Bedroom updated')
        mutate(app, a, 'remove')
        app.http('/api/radio?id=' + str(a), code=404)
        app.http('/api/command', {'id': a, 'mode': 0, 'centi_c': 2050, 'clock_seconds': 0}, 409)
    finally:
        app.stop()
    app = App(path)
    try:
        assert len(app.http()['radiators']) == 1
        assert selected(app, b)['name'] == 'Bedroom updated'
        assert app.advance(1000)['simulation_tx'] == []
        app.command(1550, 3, id=b)
        assert app.advance(1000)['command']['phase'] == 'matched'
    finally:
        app.stop()


def registry_capacity(directory):
    probe = App(Path(directory) / 'capacity-probe')
    try:
        capacity = probe.http()['capacity']
    finally:
        probe.stop()
    assert capacity >= 2
    for count in (capacity, capacity + 1):
        path = Path(directory) / ('capacity-' + str(count))
        disk = Path(str(path) + '.registry'); blob = registry_blob(count); disk.write_bytes(blob)
        # An oversized or unknown registry blocks pairing.
        app = App(path)
        try:
            s = app.advance(1000)
            if count == capacity:
                assert len(s['radiators']) == capacity and not s['registry_error']
                assert s['radiators'][-1]['id'] == capacity
            else:
                assert s['registry_error'] and s['registry_problem'] == 'stored_registry_exceeds_capacity' and s['radiators'] == []
            message = app.http('/api/pairing', {'action': 'start', 'source': 'F1010203', 'destination': 'F1100001'}, 409)
            assert 'full' in message and disk.read_bytes() == blob and s['simulation_tx'] == []
        finally:
            app.stop()


def pairing_storage_failure(directory):
    path = Path(directory) / 'pair-failure'
    blocker = Path(str(path) + '.registry.tmp'); blocker.mkdir()
    app = App(path)
    try:
        identity = pair(app, 0)
        s = app.advance(2000)
        assert s['inclusion']['phase'] == 'complete' and not s['inclusion']['saved'] and s['inclusion']['error']
        assert s['exchange_id'] == identity and s['radiators'] == []
        app.http('/api/pairing', {'action':'start','source':'F1010203','destination':'F1100001'}, 409)
        app.http('/api/pairing', {'action':'save','id':identity+1}, 409)
        tx = s['simulation_tx']
        app.http('/api/command', {'id':identity,'mode':0,'centi_c':2050,'clock_seconds':0}, 409)
        assert app.advance(1000)['simulation_tx'] == tx
        blocker.rmdir()
        app.http('/api/pairing', {'action':'save','id':identity}, 202)
        s = app.advance(1)
        assert s['inclusion']['saved'] and len(s['radiators']) == 1 and not s['exchange_id']
        assert s['simulation_tx'] == tx  # Retry persistence, not RF inclusion.
    finally:
        app.stop()


def main():
    with tempfile.TemporaryDirectory() as directory:
        two_devices(directory)
        registry_capacity(directory)
        pairing_storage_failure(directory)
    print('Two-radiator HTTP registry: independent delivery/timeouts/retries, late reports, identity routing, pairing exclusion, format/restart/removal and capacity: passed')


if __name__ == '__main__':
    main()
