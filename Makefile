BUILD_DIR ?= build
CFLAGS_TEST = -std=c11 -Wall -Wextra -Werror -pedantic -g
P = firmware/components

.PHONY: test
test:
	@mkdir -p $(BUILD_DIR)
	python3 $(P)/radiator_protocol/test/profile_vectors.py --check
	@for t in profile protocol; do \
	  $(CC) $(CPPFLAGS) $(CFLAGS_TEST) -I$(P)/radiator_protocol/include $(P)/radiator_protocol/radiator_protocol.c \
	    $(P)/radiator_protocol/test/test_$$t.c -o $(BUILD_DIR)/test_$$t && ./$(BUILD_DIR)/test_$$t || exit 1; done
	$(CC) $(CPPFLAGS) $(CFLAGS_TEST) -I$(P)/radiator_protocol/include $(P)/radiator_protocol/radiator_protocol.c \
	  $(P)/radiator_protocol/radiator_pairing.c $(P)/radiator_protocol/test/test_pairing.c -o $(BUILD_DIR)/test_pairing
	./$(BUILD_DIR)/test_pairing
	$(CC) $(CPPFLAGS) $(CFLAGS_TEST) -I$(P)/radiator_protocol/include $(P)/radiator_protocol/radiator_protocol.c \
	  $(P)/radiator_protocol/radiator_control.c $(P)/radiator_protocol/test/test_control.c -o $(BUILD_DIR)/test_control
	./$(BUILD_DIR)/test_control
	$(CC) $(CPPFLAGS) $(CFLAGS_TEST) -Ihost/include -I$(P)/device_store/include -I$(P)/radiator_protocol/include \
	  $(P)/device_store/device_store.c host/storage.c $(P)/device_store/test/test_store.c -o $(BUILD_DIR)/test_store
	./$(BUILD_DIR)/test_store
	$(CC) $(CPPFLAGS) $(CFLAGS_TEST) -Ihost/include -I$(P)/device_time/include -I$(P)/device_store/include \
	  -I$(P)/radiator_protocol/include $(P)/device_time/device_time.c $(P)/device_store/device_store.c host/storage.c \
	  $(P)/device_time/test/test_time.c -o $(BUILD_DIR)/test_time
	./$(BUILD_DIR)/test_time
	node -e "const s=require('fs').readFileSync('firmware/main/index.html','utf8'); new Function(s.split('<script>')[1].split('</script>')[0]); console.log('Page script: syntax ok')"

# Host simulation of the production firmware; needs cJSON (libcjson-dev).
HOST_CFLAGS = -Wall -Wextra -Werror -g -fPIC -MMD -MP -DCLEVERBOX_HOST -Ihost/include -Ifirmware/main \
  -I$(P)/radio_transport -I$(P)/radio_transport/include -I$(P)/device_store/include -I$(P)/device_time/include \
  -I$(P)/network_setup/include -I$(P)/mqtt_bridge/include -I$(P)/radiator_protocol/include $(shell pkg-config --cflags libcjson)
HOST_CSOURCES = $(P)/radiator_protocol/radiator_protocol.c $(P)/radiator_protocol/radiator_pairing.c \
  $(P)/radiator_protocol/radiator_control.c $(P)/device_store/device_store.c $(P)/device_time/device_time.c \
  $(P)/network_setup/network_setup.c $(P)/mqtt_bridge/mqtt_bridge.c firmware/main/http_api.c \
  host/storage.c host/network.c host/time.c host/mqtt.c host/http.c
HOST_OBJECTS = $(patsubst %.c,$(BUILD_DIR)/host/%.o,$(HOST_CSOURCES)) $(BUILD_DIR)/host/simulation.o

$(BUILD_DIR)/host/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -std=c11 $(HOST_CFLAGS) -c $< -o $@

$(BUILD_DIR)/host/simulation.o: host/simulation.cpp $(P)/radio_transport/radio_transport.cpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) -std=c++17 $(HOST_CFLAGS) -c $< -o $@

$(BUILD_DIR)/host/libcleverbox.so: $(HOST_OBJECTS)
	$(CXX) -shared $^ $(shell pkg-config --libs libcjson) -o $@

-include $(HOST_OBJECTS:.o=.d)

HOST_ENV = CLEVERBOX_SIM_LIBRARY=$(abspath $(BUILD_DIR)/host/libcleverbox.so)
.PHONY: host simulate test-http test-mqtt-broker test-home-assistant test-radiolib
host: $(BUILD_DIR)/host/libcleverbox.so

simulate: host
	$(HOST_ENV) python3 host/serve.py

test-http: host
	$(HOST_ENV) python3 host/test_http.py
	$(HOST_ENV) python3 host/test_radio.py
	$(HOST_ENV) python3 host/test_settings.py
	$(HOST_ENV) python3 host/test_mqtt.py

# Real Mosquitto 2.x broker; needs paho-mqtt==2.1.0.
test-mqtt-broker: host
	$(HOST_ENV) python3 host/test_mqtt_broker.py

# Real Home Assistant Core and Mosquitto; needs host/requirements-ha.txt.
test-home-assistant: host
	$(HOST_ENV) python3 host/test_home_assistant.py

# Real RadioLib CC1101 driver against a register model; no ESP-IDF or radio.
test-radiolib:
	@test -n "$(RADIOLIB_SOURCE)" || (echo 'Set RADIOLIB_SOURCE to an exact checkout'; exit 1)
	cmake -S test/radiolib -B $(BUILD_DIR)/radiolib -DRADIOLIB_SOURCE=$(abspath $(RADIOLIB_SOURCE))
	cmake --build $(BUILD_DIR)/radiolib --parallel 4
	./$(BUILD_DIR)/radiolib/test_cc1101
