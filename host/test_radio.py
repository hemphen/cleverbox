#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Receive path and radio errors, against the simulated CC1101 FIFO."""
from pathlib import Path
import tempfile
from test_http import App, REPORT, POLL, seed_pairing, report_from


def receive(directory):
    storage = Path(directory) / 'receive'
    app = App(storage)
    try:
        app.sim('inject', frame=REPORT)
        s = app.advance(5)
        assert s['valid_frames'] == 1 and s['last_frame'] == REPORT
        o = s['observations'][0]
        assert (o['radiator'], o['controller'], o['temperature_c'], o['target_c'], o['mode'], o['stale']) == \
            ('F1040506', 'F1010203', 20, 21, 0, False)
        # Bad CRC and a too-short length byte are counted invalid.
        for frame in (REPORT[:-2] + '00', '05' + 'AA' * 7):
            app.sim('inject', frame=frame)
            app.advance(5)
        s = app.http()
        assert s['valid_frames'] == 1 and s['invalid_frames'] == 2 and s['receive_errors'] == 0
        # A truncated frame times out after 100 ms; reception then continues.
        app.sim('inject', frame=REPORT[:40])
        assert app.advance(50)['receive_errors'] == 0
        assert app.advance(60)['receive_errors'] == 1
        app.sim('inject', frame=report_from('F1040507'))
        s = app.advance(5)
        assert s['valid_frames'] == 2 and len(s['observations']) == 2
        # Observations age and become stale after five minutes.
        o = app.advance(300000)['observations'][0]
        assert o['stale'] and o['age_ms'] >= 300000
        assert app.http()['simulation_tx'] == []
    finally:
        app.stop()


def errors(directory):
    storage = Path(directory) / 'errors'
    seed_pairing(storage)
    app = App(storage)
    try:
        app.set_time()
        app.http('/api/radio', {'action': 'retry'}, 409)  # Already ready.
        # A receive error stops the radio and ends the running command.
        app.sim('scenario', name='silence')
        app.command()
        assert app.advance(2)['radiators'][0]['command']['phase'] == 'waiting_report'
        app.sim('radio_fault', stage='receive', code=-2)
        app.sim('inject', frame=REPORT)
        s = app.advance(5)
        assert not s['ready'] and s['error'] == -2 and s['error_stage'] == 'receive'
        assert s['radiators'][0]['command']['phase'] == 'error' and s['radiators'][0]['command']['error'] == -2
        assert app.command(code=409)['error'] == 'radio_not_ready'
        assert app.http('/api/pairing', {'action': 'start', 'source': 'F1010203', 'destination': 'F1100001'}, 409)['error'] == 'radio_not_ready'
        tx = app.http()['simulation_tx']
        # Retry while the fault persists fails during setup; nothing is sent.
        app.http('/api/radio', {'action': 'retry'}, 202)
        s = app.advance(10)
        assert not s['ready'] and s['error_stage'] == 'init' and s['simulation_tx'] == tx
        app.sim('radio_fault', stage='receive', code=0)
        app.http('/api/radio', {'action': 'retry'}, 202)
        s = app.advance(10)
        assert s['ready'] and s['error_stage'] == 'init' and s['simulation_tx'] == tx  # Last error stays visible.
        assert s['radiators'][0]['command']['phase'] == 'error'  # Not resumed.
        # A transmit error is reported for the command and stops the radio.
        app.sim('scenario', name='normal')
        app.sim('radio_fault', stage='transmit', code=-5)
        app.command()
        s = app.advance(5)
        assert not s['ready'] and s['error_stage'] == 'transmit'
        assert s['radiators'][0]['command']['phase'] == 'error' and s['radiators'][0]['command']['error'] == -5
        app.sim('radio_fault', stage='transmit', code=0)
        app.http('/api/radio', {'action': 'retry'}, 202)
        assert app.advance(10)['ready']
        app.command()
        s = app.advance(1000)
        assert s['radiators'][0]['command']['phase'] == 'matched' and POLL in s['simulation_tx']
    finally:
        app.stop()


def main():
    with tempfile.TemporaryDirectory() as directory:
        receive(directory)
        errors(directory)
    print('Radio: report decoding, invalid and truncated frames, receive/transmit errors and manual retry: passed')


if __name__ == '__main__':
    main()
