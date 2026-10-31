#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Faults affect only adapter I/O; all recovery/admission/publication is production."""
from pathlib import Path
import tempfile
from test_http import App, REPORT, STATE
from test_http_boundary import request
from test_passive_ha import select, setup, enable, rows, STATE_TOPIC
from test_mqtt import BASE, connect, publications


def fault(app, stage, code):
    app.sim('radio_fault', stage=stage, code=code)


def exhaust(app):
    fault(app, 'initialize', -42)
    fault(app, 'spi', -61)
    app.advance(1)
    fault(app, 'spi', 0)
    app.advance(10000)
    s=app.http()
    assert s['health']=='faulted' and s['recovery_attempt']==4 and not s['ready'] and s['retry_allowed']
    before=s['recovery_attempts']; app.advance(600000)
    assert app.http()['recovery_attempts']==before
    return s


def run(directory):
    storage=directory/'initial'
    app=App(storage, operating_mode=None, interfaces=True, radio_init_fault=-42)
    try:
        s=app.http(); assert not s['ready'] and s['health']=='initializing' and s['recovery_attempt']==1
        assert s['retry_in_ms']==1000 and not s['control_ready']
        app.http('/api/radio',dict(action='retry'),409)
        app.http('/api/radio',dict(action='retry',extra=True),400)
        app.http('/api/radio',raw=b'{"action":"retry","action":"retry"}',code=400)
        app.advance(999); assert app.http()['recovery_attempt']==1
        fault(app,'initialize',0); app.advance(1)
        assert app.http()['ready'] and app.http()['recovery_successes']==1
        exhaust(app)
        for host in ('127.0.0.2','127.0.0.3'):
            request(app,host,'/api/radio',dict(action='retry'),403)
        # Explicit retry also works after failed boot. Repeated HTTP requests cannot
        # overlap batches, and local writes are independent of radio readiness.
        app.http('/api/radio',dict(action='retry'),202)
        app.http('/api/radio',dict(action='retry'),409)
        app.http('/api/commissioning',dict(mode='listen-only'),202)
        fault(app,'initialize',0); app.advance(1)
        s=app.http(); assert s['ready'] and s['recovery_successes']==2 and s['simulation_tx']==[]
        assert app.http('/api/commissioning')['mode']=='listen-only'
        assert app.http('/api/network') and app.http('/api/mqtt')
    finally: app.stop()
    # Full passive identity/age semantics, outage and MQTT reconnect without RF.
    storage=directory/'passive'; app=App(storage,operating_mode=None)
    try:
        app.sim('inject',frame=REPORT); app.advance(1); select(app); setup(app); enable(app)
        before=app.http('/api/commissioning')['monitors'][0]; stamp=before['age_ms']
        durable=Path(str(storage)+'.ha_monitors').read_bytes()
        exhaust(app)
        snapshots=rows(app,100)
        state=[x['payload'] for x in snapshots if x['topic']==STATE_TOPIC][-1]
        bridge=[x['payload'] for x in snapshots if x['topic']==BASE+'/bridge'][-1]
        assert state['available'] and not state['fresh'] and not state['radio_ready']
        assert state['age_ms']>stamp and state['temperature_centi_c']==2000
        assert state['radio_health']=='faulted' and bridge['retry_exhausted'] and not bridge['control_ready']
        age=state['age_ms']; app.http('/api/radio',dict(action='retry'),202)
        fault(app,'initialize',0); app.advance(1)
        state=[x['payload'] for x in rows(app,100) if x['topic']==STATE_TOPIC][-1]
        assert state['radio_ready'] and state['age_ms']>=age and not state['fresh'] # expired while faulted
        assert Path(str(storage)+'.ha_monitors').read_bytes()==durable
        app.sim('inject',frame=REPORT); app.advance(1)
        assert [x['payload'] for x in rows(app,5100) if x['topic']==STATE_TOPIC][-1]['fresh']
        tx=app.http()['simulation_tx']; assert tx==[]
        app.sim('mqtt_connection',connected=False); app.advance(6000); connect(app); app.advance(5100)
        assert app.http()['simulation_tx']==tx
    finally: app.stop()
    app=App(storage,operating_mode=None)
    try:
        s=app.http('/api/commissioning'); assert len(s['monitors'])==1 and not s['monitors'][0]['available']
        assert app.http()['ready'] and app.http()['simulation_tx']==[]
    finally: app.stop()
    # Failure at TX may have reached the radiator. All independent commands end;
    # recovery/reconnect/reboot do not send them again.
    storage=directory/'commands'; storage.write_bytes(bytes.fromhex('01F1010203F1040506'))
    app=App(storage)
    try:
        app.http('/api/controller-state',dict(action='import',state=STATE),202); app.advance(1)
        setup(app)
        app.sim('scenario',name='silence'); fault(app,'transmit',-99)
        app.command(); s=app.advance(1)
        assert s['command']['phase']=='error' and s['command']['error']==0x730A
        assert s['failure_stage']=='transmit' and s['failure_code']==-99 and not s['active_commands']
        fault(app,'transmit',0); app.advance(1000); tx=app.http()['simulation_tx']; app.advance(10000)
        assert len(tx)==1 and app.http()['simulation_tx']==tx and app.http()['ready']
        old_session=app.http('/api/mqtt')['session']
        app.sim('mqtt_connection',connected=False); app.advance(6000); connect(app); app.advance(5100)
        assert app.http('/api/mqtt')['session']!=old_session
        assert app.http()['command']['phase']=='error' and app.http()['simulation_tx']==tx
        # New independent command can reserve the same radiator.
        app.command(); app.advance(1); assert app.http()['command']['pending']
        # RX restore failure terminates a queued/active inclusion honestly.
    finally: app.stop()
    app=App(storage)
    try: assert app.http()['simulation_tx']==[] and app.http()['command']['phase']=='idle'
    finally: app.stop()


if __name__=='__main__':
    with tempfile.TemporaryDirectory() as directory: run(Path(directory))
    print('Radio health: failed boot/retry bounds/STA admission, uncertainty, persistence, passive identity/age, MQTT/reboot no replay: passed')
