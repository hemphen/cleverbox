# SPDX-License-Identifier: MIT
"""Real broker connection for the host simulation (serve.py --real-mqtt).

Implements the C mqtt_client_ops boundary with paho-mqtt==2.1.0. Paho runs only
when serve.py calls poll(), on the thread that owns the C application.
"""
import ctypes as C
import time
import paho.mqtt.client as mqtt


class Settings(C.Structure):
    _fields_ = [('enabled', C.c_bool), ('discovery', C.c_bool), ('port', C.c_uint16),
                ('host', C.c_char * 97), ('username', C.c_char * 65), ('password', C.c_char * 129)]


START = C.CFUNCTYPE(C.c_int, C.POINTER(Settings), C.c_char_p, C.c_char_p)
STOP = C.CFUNCTYPE(None)
PUBLISH = C.CFUNCTYPE(C.c_bool, C.c_char_p, C.c_char_p, C.c_int, C.c_bool)
SUBSCRIBE = C.CFUNCTYPE(C.c_bool, C.c_char_p)


class Ops(C.Structure):
    _fields_ = [('start', START), ('stop', STOP), ('publish', PUBLISH), ('subscribe', SUBSCRIBE)]


class BrokerClient:
    def __init__(self, lib):
        self.lib, self.client, self.connected = lib, None, False
        self.ops = Ops(START(self.start), STOP(self.stop), PUBLISH(self.publish), SUBSCRIBE(self.subscribe))
        lib.mqtt_bridge_init.argtypes = [Ops, C.c_char_p]
        lib.mqtt_bridge_connected.argtypes = [C.c_bool]
        lib.mqtt_bridge_init(self.ops, b'020000000001')

    def start(self, settings, identity, will):
        s = settings.contents
        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=identity.decode(),
                                  clean_session=True, protocol=mqtt.MQTTv311)
        if s.username:
            self.client.username_pw_set(s.username.decode(), s.password.decode())
        self.client.will_set(will.decode(), 'offline', qos=1, retain=True)
        self.client.reconnect_delay_set(1, 5)
        self.client.on_connect = lambda c, u, f, reason, p: self.changed(reason == 0)
        self.client.on_disconnect = lambda c, u, f, reason, p: self.changed(False)
        self.client.on_message = self.on_message
        self.client.connect_async(s.host.decode(), s.port, 30)
        self.retry_at = 0
        return 0

    def changed(self, connected):
        self.connected = connected
        self.lib.mqtt_bridge_connected(connected)

    def on_message(self, client, userdata, message):
        self.lib.mqtt_bridge_message(message.topic.encode(), message.payload, len(message.payload), message.retain)

    def subscribe(self, topic):
        return self.connected and self.client.subscribe(topic.decode(), qos=0)[0] == mqtt.MQTT_ERR_SUCCESS

    def publish(self, topic, payload, qos, retain):
        return self.connected and self.client.publish(topic.decode(), payload, qos=qos, retain=retain).rc == mqtt.MQTT_ERR_SUCCESS

    def stop(self):
        if self.client:
            self.client.disconnect()
            self.client.loop(timeout=0.01)
        self.client, self.connected = None, False

    def poll(self):
        if not self.client:
            return
        if self.client.loop(timeout=0.001) != mqtt.MQTT_ERR_SUCCESS and time.monotonic() >= self.retry_at:
            self.retry_at = time.monotonic() + 1  # Reconnect by itself, like ESP-MQTT.
            try:
                self.client.reconnect()
            except OSError:
                pass
