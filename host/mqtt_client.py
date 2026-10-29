# SPDX-License-Identifier: MIT
"""Optional Paho I/O boundary for the production C MQTT service. No routing here.

Requires paho-mqtt==2.1.0. Call poll() from the simulator owner, never loop_start().
"""
import ctypes as C
import secrets
import time
import paho.mqtt.client as mqtt


class Settings(C.Structure):
    _fields_=[('configured',C.c_bool),('enabled',C.c_bool),('port',C.c_uint16),
              ('host',C.c_char*97),('username',C.c_char*65),('password',C.c_char*129)]


START=C.CFUNCTYPE(C.c_int,C.POINTER(Settings),C.c_char_p,C.c_char_p)
STOP=C.CFUNCTYPE(None,C.c_char_p)
SUBSCRIBE=C.CFUNCTYPE(C.c_int,C.c_char_p)
PUBLISH=C.CFUNCTYPE(C.c_bool,C.c_char_p,C.c_char_p,C.c_bool)
NONCE=C.CFUNCTYPE(None,C.POINTER(C.c_char))


class Ops(C.Structure):
    _fields_=[('start',START),('stop',STOP),('subscribe',SUBSCRIBE),('publish',PUBLISH),('nonce',NONCE)]


class BrokerClient:
    def __init__(self,lib):
        self.lib=lib; self.client=None; self.connected=False; self.reconnect_at=0
        self.ops=Ops(START(self.start),STOP(self.stop),SUBSCRIBE(self.subscribe),PUBLISH(self.publish),NONCE(self.nonce))
        lib.mqtt_bridge_init.argtypes=[Ops,C.c_char_p]
        lib.mqtt_bridge_input.argtypes=[C.c_char_p,C.c_size_t,C.c_char_p,C.c_size_t,C.c_size_t,C.c_size_t,C.c_bool]
        lib.mqtt_bridge_connection.argtypes=[C.c_bool]
        lib.mqtt_bridge_suback.argtypes=[C.c_int,C.c_int]
        lib.mqtt_bridge_init(self.ops,b'020000000001')

    def start(self,settings,identity,will):
        s=settings.contents
        self.client=mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,client_id=identity.decode(),clean_session=True,protocol=mqtt.MQTTv311)
        if s.username: self.client.username_pw_set(s.username.decode(),s.password.decode())
        self.client.will_set(will.decode(),'offline',qos=1,retain=True)
        self.client.connect_timeout=1
        self.reconnect_at=time.monotonic()+5
        self.client.on_subscribe=self.on_subscribe
        self.client.on_connect=self.on_connect; self.client.on_disconnect=self.on_disconnect; self.client.on_message=self.on_message
        self.client.reconnect_delay_set(5,5)
        try: self.client.connect(s.host.decode(),s.port,30)
        except OSError: self.client=None; return -1
        return 0

    def on_connect(self,client,userdata,flags,reason,properties):
        self.connected=reason==0; self.reconnect_at=time.monotonic()+5; self.lib.mqtt_bridge_connection(self.connected)

    def on_disconnect(self,client,userdata,flags,reason,properties):
        self.connected=False; self.reconnect_at=time.monotonic()+5; self.lib.mqtt_bridge_connection(False)

    def on_message(self,client,userdata,message):
        topic=message.topic.encode(); payload=message.payload
        self.lib.mqtt_bridge_input(topic,len(topic),payload,len(payload),0,len(payload),message.retain)

    def on_subscribe(self,client,userdata,mid,reasons,properties):
        granted=len(reasons)==1 and not reasons[0].is_failure and reasons[0].value==0
        self.lib.mqtt_bridge_suback(mid,0 if granted else -1)

    def subscribe(self,topic):
        if not self.connected: return -1
        error,mid=self.client.subscribe(topic.decode(),qos=0)
        return mid if error==mqtt.MQTT_ERR_SUCCESS else -1

    def publish(self,topic,payload,retain):
        return self.connected and self.client.publish(topic.decode(),payload,qos=0,retain=retain).rc==mqtt.MQTT_ERR_SUCCESS

    def nonce(self,out):
        C.memmove(out,secrets.token_hex(16).encode()+b'\0',33)

    def stop(self,will):
        if self.client:
            if self.connected:
                self.publish(will,b'offline',True)
                self.client.loop_write()
                self.client.disconnect()
                self.client.loop_write()
            self.client=None
        self.connected=False

    def poll(self):
        if self.client:
            if not self.connected and time.monotonic() >= self.reconnect_at:
                self.reconnect_at=time.monotonic()+5
                try: self.client.reconnect()
                except OSError: return
            self.client.loop(timeout=0.001)
