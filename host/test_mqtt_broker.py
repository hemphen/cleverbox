#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Real Mosquitto/Paho host smoke test; production routing, HTTP/time and RF.

Needs mosquitto on PATH and paho-mqtt==2.1.0. Broker binds loopback only.
This tests real MQTT packets via a host adapter, not ESP-IDF's socket adapter.
"""
import ctypes as C
import json
import os
from pathlib import Path
import shutil
import pwd
import socket
import subprocess
import tempfile
import time
import paho.mqtt.client as mqtt
from mqtt_client import BrokerClient
from test_http import ROOT,STATE

BASE='cleverbox/v1/020000000001'


def run(directory):
    directory=Path(directory); storage=directory/'broker.bin'; storage.write_bytes(bytes.fromhex('01F1010203F1040506'))
    os.environ['CLEVERBOX_SIM_STORAGE']=str(storage)
    sock=socket.socket(); sock.bind(('127.0.0.1',0)); port=sock.getsockname()[1]; sock.close()
    conf=directory/'mosquitto.conf'; conf.write_text(f'listener {port} 127.0.0.1\nallow_anonymous true\npersistence false\nuser {pwd.getpwuid(os.geteuid()).pw_name}\n')
    log=open(directory/'broker.log','wb')
    def broker(): return subprocess.Popen([shutil.which('mosquitto'),'-c',str(conf)],stdout=log,stderr=log)
    process=broker()
    lib=C.CDLL(os.environ.get('CLEVERBOX_SIM_LIBRARY',str(ROOT/'build/host/libcleverbox.so')))
    lib.host_request.argtypes=[C.c_int,C.c_char_p,C.c_char_p,C.c_char_p]; lib.host_response.restype=C.c_char_p
    lib.simulation_advance.argtypes=[C.c_int]; lib.host_network_event.argtypes=[C.c_char_p]
    lib.simulation_scenario.argtypes=[C.c_char_p]
    lib.simulation_radio_fault.argtypes=[C.c_char_p,C.c_int]
    lib.host_init(); adapter=BrokerClient(lib)
    # Direct handler calls still need a real trusted local socket context.
    # Keep both TCP ends open; netif addresses are the host loopback fixtures.
    listener=socket.create_server(('127.0.0.1',0))
    http_peer=socket.create_connection(listener.getsockname())
    http_socket,_=listener.accept(); listener.close()
    lib.host_http_socket(http_socket.fileno())
    def http(path='/api/radio',body=None,code=200):
        raw=json.dumps(body).encode() if body is not None else b''
        result=lib.host_request(body is not None,path.encode(),raw,b'application/json'); text=lib.host_response()
        assert result==code,(result,code,text)
        return json.loads(text)
    messages=[]
    observer=mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,client_id='cleverbox-smoke-observer')
    def on_message(client,userdata,message):
        value=message.payload.decode(); messages.append((message.topic,json.loads(value) if value.startswith('{') else value,message.retain))
    observer.on_message=on_message
    observer.on_connect=lambda c,u,f,r,p:c.subscribe(BASE+'/#')
    def pump(predicate=lambda:False,timeout=4):
        until=time.monotonic()+timeout
        while time.monotonic()<until:
            adapter.poll(); observer.loop(timeout=0.001); lib.simulation_advance(10)
            if predicate(): return
            time.sleep(0.002)
        assert predicate(),messages[-8:]
    try:
        # Wait for the real process to bind (no assumed startup delay).
        until=time.monotonic()+3
        while True:
            try: observer.connect('127.0.0.1',port,30); break
            except ConnectionRefusedError:
                assert process.poll() is None and time.monotonic()<until,(directory/'broker.log').read_text(); time.sleep(.02)
        observer.subscribe(BASE+'/#')
        http('/api/mqtt',dict(action='configure',host='127.0.0.1',port=port,username='',password='',enabled=True),202)
        # Fake Wi-Fi boundary only; actual broker traffic is a TCP loopback socket.
        import secrets
        http('/api/network',dict(action='configure',ssid=secrets.token_hex(6),password=secrets.token_hex(8)),202)
        lib.host_network_event(b'started'); lib.host_network_event(b'got_ip')
        pump(lambda:http('/api/mqtt')['connected'])
        pump(lambda:any(t.endswith('/state') for t,p,r in messages))
        assert any(t.endswith('/state') and not p['available'] for t,p,r in messages)
        assert http()['simulation_tx']==[]
        assert not http('/api/radio')['command']['template_available']
        http('/api/time',dict(action='manual',unix_seconds=1767268800))
        session=http('/api/mqtt')['session']
        payload=json.dumps(dict(session=session,request_id=1,mode=0,centi_c=2150))
        observer.publish(BASE+'/radiators/1/set',payload,qos=1)
        pump(lambda:any(isinstance(p,dict) and p.get('phase')=='matched' for t,p,r in messages))
        assert any(isinstance(p,dict) and p.get('stage')=='admission' and p['accepted'] for t,p,r in messages)
        assert http()['observed']['target_c']==21.5
        tx=http()['simulation_tx']
        observer.publish(BASE+'/radiators/1/set',payload,qos=1)
        pump(lambda:any(isinstance(p,dict) and p.get('reason')=='duplicate_or_out_of_order' for t,p,r in messages))
        assert http()['simulation_tx']==tx
        # Seed an old retained command; it is never admitted on reconnect.
        observer.publish(BASE+'/radiators/1/set',payload,qos=1,retain=True)
        pump(lambda:len([p for t,p,r in messages if isinstance(p,dict) and p.get('reason')=='duplicate_or_out_of_order'])>=2)
        before_offline=len([p for t,p,r in messages if p=='offline'])
        http('/api/mqtt',dict(action='disable'),202)
        pump(lambda:not http('/api/mqtt')['pending'] and not http('/api/mqtt')['enabled'] and
             len([p for t,p,r in messages if p=='offline'])>before_offline)
        http('/api/mqtt',dict(action='enable'),202)
        pump(lambda:http('/api/mqtt')['connected'] and http('/api/mqtt')['session']!=session)
        pump(lambda:any(isinstance(p,dict) and p.get('reason')=='retained_command' for t,p,r in messages))
        assert http()['simulation_tx']==tx
        # Abrupt TCP close, without DISCONNECT, exercises the broker's actual will.
        before_offline=len([p for t,p,r in messages if p=='offline'])
        adapter.client.socket().shutdown(socket.SHUT_RDWR)
        pump(lambda:len([p for t,p,r in messages if p=='offline'])>before_offline)
        previous=http('/api/mqtt')['session']
        pump(lambda:http('/api/mqtt')['connected'] and http('/api/mqtt')['session']!=previous,timeout=10)
        assert http()['simulation_tx']==tx
        # Broker process loss/restart: RF finishes independently, no reconnect replay.
        new_session=http('/api/mqtt')['session']; lib.simulation_scenario(b'delayed')
        observer.publish(BASE+'/radiators/1/set',json.dumps(dict(session=new_session,request_id=1,mode=3,centi_c=1550)))
        pump(lambda:http()['command']['pending'])
        process.terminate(); process.wait(timeout=3)
        # Poll socket disconnect, then advance RF independently of any broker.
        for _ in range(10): adapter.poll(); lib.simulation_advance(1000)
        assert http()['command']['phase']=='matched' and http()['observed']['target_c']==15.5
        tx=http()['simulation_tx']; process=broker()
        until=time.monotonic()+3
        while True:
            try: observer.reconnect(); break
            except ConnectionRefusedError:
                assert process.poll() is None and time.monotonic()<until; time.sleep(.02)
        # The production owner recreates either adapter with a fresh outbox.
        pump(lambda:http('/api/mqtt')['connected'] and http('/api/mqtt')['session']!=new_session,timeout=10)
        assert http()['simulation_tx']==tx
        # Actual broker remains connected throughout the radio-only outage.
        tx=http()['simulation_tx']; messages.clear()
        lib.simulation_radio_fault(b'initialize',-42); lib.simulation_radio_fault(b'spi',-61)
        lib.simulation_advance(1); lib.simulation_radio_fault(b'spi',0)
        lib.simulation_advance(10000)
        pump(lambda:any(t==BASE+'/bridge' and p.get('radio_health')=='faulted' for t,p,r in messages))
        assert http('/api/mqtt')['connected'] and http()['health']=='faulted'
        assert any(t.endswith('/state') and not p['fresh'] and not p['radio_ready'] for t,p,r in messages)
        http('/api/radio',dict(action='retry'),202); lib.simulation_radio_fault(b'initialize',0)
        messages.clear()
        pump(lambda:any(t==BASE+'/bridge' and p.get('radio_ready') for t,p,r in messages))
        assert http()['simulation_tx']==tx and http()['recovery_successes']==1
        http('/api/mqtt',dict(action='forget'),202); pump(lambda:not http('/api/mqtt')['configured'])
        assert http()['pairing']['saved'] and http('/api/time')['usable']
        print('Real Mosquitto 2.x / Paho 2.1.0 smoke: TCP publications, command acceptance/readback, duplicates, retained rejection, offline, broker restart and RF independence: passed')
    finally:
        if adapter.client: adapter.stop((BASE+'/availability').encode())
        observer.disconnect()
        process.terminate(); process.wait(timeout=3); log.close()
        http_socket.close(); http_peer.close()


if __name__=='__main__':
    if not shutil.which('mosquitto'): raise SystemExit('Install mosquitto and paho-mqtt==2.1.0 for this optional smoke test')
    with tempfile.TemporaryDirectory() as directory: run(directory)
