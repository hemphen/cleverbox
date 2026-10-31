BUILD_DIR ?= build
.PHONY: test
test:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -std=c11 -Wall -Wextra -Werror -pedantic -g \
	  -Ifirmware/components/radiator_protocol/include \
	  firmware/components/radiator_protocol/radiator_protocol.c \
	  firmware/components/radiator_protocol/test/test_protocol.c -o $(BUILD_DIR)/test_protocol
	./$(BUILD_DIR)/test_protocol
	$(CC) $(CPPFLAGS) -std=c11 -Wall -Wextra -Werror -pedantic -g \
	  -Ifirmware/components/radiator_protocol/include \
	  firmware/components/radiator_protocol/radiator_protocol.c \
	  firmware/components/radiator_protocol/radiator_pairing.c \
	  firmware/components/radiator_protocol/test/test_pairing.c -o $(BUILD_DIR)/test_pairing
	./$(BUILD_DIR)/test_pairing
	$(CC) $(CPPFLAGS) -std=c11 -Wall -Wextra -Werror -pedantic -g \
	  -Ifirmware/components/device_store/include \
	  -Ifirmware/components/device_store/test/stubs \
	  -Ifirmware/components/radiator_protocol/include \
	  firmware/components/radiator_protocol/radiator_protocol.c \
	  firmware/components/device_store/device_store.c host/test_mode_gate.c \
	  firmware/components/device_store/test/test_store.c -o $(BUILD_DIR)/test_store
	./$(BUILD_DIR)/test_store
	$(CC) $(CPPFLAGS) -std=c11 -Wall -Wextra -Werror -pedantic -g \
	  -Ihost/include -Ifirmware/components/device_store/include -Ifirmware/components/radiator_protocol/include \
	  firmware/components/device_store/mode_store.c \
	  firmware/components/device_store/test/test_mode_store.c -o $(BUILD_DIR)/test_mode_store
	./$(BUILD_DIR)/test_mode_store
	$(CC) $(CPPFLAGS) -std=c11 -Wall -Wextra -Werror -pedantic -g -pthread \
	  -Ihost/include -Ifirmware/components/device_store/include \
	  firmware/components/device_store/mode_store.c \
	  firmware/components/device_store/test/test_mode_bootstrap.c -o $(BUILD_DIR)/test_mode_bootstrap
	./$(BUILD_DIR)/test_mode_bootstrap
	./$(BUILD_DIR)/test_mode_bootstrap race
	./$(BUILD_DIR)/test_mode_bootstrap enable-first
	$(CC) $(CPPFLAGS) -std=c11 -Wall -Wextra -Werror -pedantic -g \
	  -Ihost/include -Ifirmware/components/device_store/include -Ifirmware/components/radiator_protocol/include \
	  firmware/components/device_store/monitor_store.c firmware/components/device_store/test/test_monitors.c -o $(BUILD_DIR)/test_monitors
	./$(BUILD_DIR)/test_monitors
	$(CC) $(CPPFLAGS) -std=c11 -Wall -Wextra -Werror -pedantic -g \
	  -Ifirmware/components/radiator_protocol/include \
	  firmware/components/radiator_protocol/radiator_protocol.c \
	  firmware/components/radiator_protocol/radiator_control.c \
	  firmware/components/radiator_protocol/test/test_control.c -o $(BUILD_DIR)/test_control
	./$(BUILD_DIR)/test_control
	@for header in RadioLib.h esp_err.h esp_log.h esp_timer.h esp_rom_sys.h \
	  driver/gpio.h driver/spi_master.h freertos/FreeRTOS.h freertos/task.h freertos/queue.h freertos/semphr.h esp_random.h; do \
	  mkdir -p $(BUILD_DIR)/transport_stubs/$$(dirname $$header); \
	  printf '#include "transport_stubs.h"\n' > $(BUILD_DIR)/transport_stubs/$$header; done
	$(CC) $(CPPFLAGS) -std=c11 -Ifirmware/components/radiator_protocol/include \
	  -c firmware/components/radiator_protocol/radiator_protocol.c -o $(BUILD_DIR)/transport_protocol.o
	$(CC) $(CPPFLAGS) -std=c11 -Ifirmware/components/radiator_protocol/include \
	  -c firmware/components/radiator_protocol/radiator_pairing.c -o $(BUILD_DIR)/transport_pairing.o
	$(CC) $(CPPFLAGS) -std=c11 -Ifirmware/components/radiator_protocol/include \
	  -c firmware/components/radiator_protocol/radiator_control.c -o $(BUILD_DIR)/transport_control.o
	$(CXX) $(CPPFLAGS) -std=c++17 -Wall -Wextra -Werror -pedantic -g \
	  -I$(BUILD_DIR)/transport_stubs -Ifirmware/components/radio_transport/test/stubs \
	  -Ifirmware/components/device_time/include \
	  -Ifirmware/components/radio_transport/include -Ifirmware/components/device_store/include \
	  -Ifirmware/components/radiator_protocol/include \
	  firmware/components/radio_transport/test/test_admission.cpp \
	  $(BUILD_DIR)/transport_protocol.o $(BUILD_DIR)/transport_pairing.o $(BUILD_DIR)/transport_control.o -o $(BUILD_DIR)/test_admission
	./$(BUILD_DIR)/test_admission
	$(CC) $(CPPFLAGS) -std=c11 -Wall -Wextra -Werror -pedantic -g \
	  -Ihost/include -Ifirmware/components/network_setup/include -Ifirmware/components/device_store/include \
	  firmware/components/network_setup/network_policy.c \
	  firmware/components/network_setup/test/test_network.c -o $(BUILD_DIR)/test_network
	./$(BUILD_DIR)/test_network
	$(CC) $(CPPFLAGS) -std=c11 -Wall -Wextra -Werror -pedantic -g \
	  -Ifirmware/components/network_setup/include \
	  firmware/components/network_setup/network_button.c \
	  firmware/components/network_setup/test/test_button.c -o $(BUILD_DIR)/test_button
	./$(BUILD_DIR)/test_button
	@for header in esp_event.h esp_netif.h esp_wifi.h esp_timer.h esp_log.h nvs.h \
	  driver/gpio.h freertos/FreeRTOS.h freertos/task.h freertos/queue.h freertos/semphr.h; do \
	  mkdir -p $(BUILD_DIR)/network_stubs/$$(dirname $$header); \
	  printf '#include "adapter_stubs.h"\n' > $(BUILD_DIR)/network_stubs/$$header; done
	$(CC) $(CPPFLAGS) -std=c11 -Wall -Wextra -Werror -pedantic -g \
	  -I$(BUILD_DIR)/network_stubs -Ifirmware/components/network_setup/test/stubs \
	  -Ihost/include -Ifirmware/components/network_setup/include -Ifirmware/components/device_store/include \
	  firmware/components/network_setup/network_policy.c firmware/components/network_setup/network_button.c \
	  firmware/components/network_setup/test/test_adapter.c -o $(BUILD_DIR)/test_network_adapter
	./$(BUILD_DIR)/test_network_adapter
	$(CC) $(CPPFLAGS) -std=c11 -Wall -Wextra -Werror -pedantic -g \
	  -Ihost/include -Ifirmware/components/network_setup/include -Ifirmware/components/device_store/include \
	  firmware/components/network_setup/network_policy.c firmware/components/network_setup/network_store.c host/test_mode_gate.c \
	  firmware/components/network_setup/test/test_network_store.c -o $(BUILD_DIR)/test_network_store
	./$(BUILD_DIR)/test_network_store
	$(CC) $(CPPFLAGS) -std=c11 -Wall -Wextra -Werror -pedantic -g \
	  -Ihost/include -Ifirmware/components/device_time/include -Ifirmware/components/device_store/include \
	  firmware/components/device_time/device_time.c firmware/components/device_time/time_store.c host/test_mode_gate.c \
	  firmware/components/device_time/test/test_time.c -o $(BUILD_DIR)/test_time
	./$(BUILD_DIR)/test_time
	$(CC) $(CPPFLAGS) -std=c11 -Wall -Wextra -Werror -pedantic -g \
	  -Ihost/include -Ifirmware/components/device_time/include -Ifirmware/components/device_store/include \
	  firmware/components/device_time/device_time.c firmware/components/device_time/time_store.c host/test_mode_gate.c \
	  firmware/components/device_time/test/test_time_store.c -o $(BUILD_DIR)/test_time_store
	./$(BUILD_DIR)/test_time_store
	$(CC) $(CPPFLAGS) -std=c11 -Wall -Wextra -Werror -pedantic -g \
	  -Ihost/include -Ifirmware/components/mqtt_bridge/include -Ifirmware/components/device_store/include \
	  firmware/components/mqtt_bridge/mqtt_store.c host/test_mode_gate.c firmware/components/mqtt_bridge/test/test_store.c -o $(BUILD_DIR)/test_mqtt_store
	./$(BUILD_DIR)/test_mqtt_store
	$(CC) $(CPPFLAGS) -std=c11 -Wall -Wextra -Werror -pedantic -g \
	  -Ihost/include -Ifirmware/main firmware/main/http_access.c firmware/main/test_http_access.c -o $(BUILD_DIR)/test_http_access
	./$(BUILD_DIR)/test_http_access
	node firmware/main/test_ui.js

# Host simulation needs cJSON development headers/library (e.g. libcjson-dev).
HOST_CFLAGS = -std=c11 -Wall -Wextra -Werror -pedantic -g -fPIC -MMD -MP -DCLEVERBOX_HOST \
  -Ifirmware/components/mqtt_bridge/include -Ifirmware/components/device_time/include -Ifirmware/components/network_setup/include -Ihost/include -Ifirmware/main -Ifirmware/components/radio_transport \
  -Ifirmware/components/radio_transport/include -Ifirmware/components/device_store/include \
  -Ifirmware/components/radiator_protocol/include $(shell pkg-config --cflags libcjson)
HOST_CSOURCES = firmware/components/device_store/monitor_store.c firmware/components/mqtt_bridge/mqtt_discovery.c firmware/components/mqtt_bridge/mqtt_bridge.c firmware/components/mqtt_bridge/mqtt_store.c host/mqtt.c \
  firmware/components/device_time/device_time.c \
  firmware/components/device_time/time_store.c host/time.c \
  firmware/components/radiator_protocol/radiator_protocol.c \
  firmware/components/radiator_protocol/radiator_pairing.c \
  firmware/components/radiator_protocol/radiator_control.c \
  firmware/components/device_store/device_store.c firmware/components/device_store/mode_store.c firmware/main/http_api.c firmware/main/http_access.c host/netif.c host/http.c host/storage.c \
  firmware/components/network_setup/network_policy.c firmware/components/network_setup/network_button.c firmware/components/network_setup/network_store.c host/network.c
HOST_OBJECTS = $(patsubst %.c,$(BUILD_DIR)/host/%.o,$(HOST_CSOURCES))

$(BUILD_DIR)/host/%.o: %.c $(wildcard host/include/*.h) firmware/main/http_api.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(HOST_CFLAGS) -c $< -o $@

$(BUILD_DIR)/host/simulation.o: host/simulation.cpp host/include/platform.h host/include/simulation.h \
  firmware/components/radio_transport/radio_transport.cpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(filter-out -std=c11,$(HOST_CFLAGS)) -std=c++17 -c $< -o $@

$(BUILD_DIR)/host/libcleverbox.so: $(HOST_OBJECTS) $(BUILD_DIR)/host/simulation.o
	$(CXX) $(CPPFLAGS) -shared $^ $(shell pkg-config --libs libcjson) -o $@

.PHONY: host simulate test-http
host: $(BUILD_DIR)/host/libcleverbox.so
simulate: host
	CLEVERBOX_SIM_LIBRARY=$(abspath $(BUILD_DIR)/host/libcleverbox.so) python3 host/serve.py

test-http: host test-http-physical
	CLEVERBOX_SIM_LIBRARY=$(abspath $(BUILD_DIR)/host/libcleverbox.so) python3 host/test_passive_ha.py
	CLEVERBOX_SIM_LIBRARY=$(abspath $(BUILD_DIR)/host/libcleverbox.so) python3 host/test_passive.py
	CLEVERBOX_SIM_LIBRARY=$(abspath $(BUILD_DIR)/host/libcleverbox.so) python3 host/test_http.py
	CLEVERBOX_SIM_LIBRARY=$(abspath $(BUILD_DIR)/host/libcleverbox.so) python3 host/test_registry.py
	CLEVERBOX_SIM_LIBRARY=$(abspath $(BUILD_DIR)/host/libcleverbox.so) python3 host/test_network.py
	CLEVERBOX_SIM_LIBRARY=$(abspath $(BUILD_DIR)/host/libcleverbox.so) python3 host/test_recovery.py
	CLEVERBOX_SIM_LIBRARY=$(abspath $(BUILD_DIR)/host/libcleverbox.so) python3 host/test_time.py
	CLEVERBOX_SIM_LIBRARY=$(abspath $(BUILD_DIR)/host/libcleverbox.so) python3 host/test_mqtt.py
	CLEVERBOX_SIM_LIBRARY=$(abspath $(BUILD_DIR)/host/libcleverbox.so) python3 host/test_mqtt_tokens.py
	CLEVERBOX_SIM_LIBRARY=$(abspath $(BUILD_DIR)/host/libcleverbox.so) python3 host/test_mqtt_discovery.py

-include $(HOST_OBJECTS:.o=.d) $(BUILD_DIR)/host/simulation.d

# Compile and exercise the same production application with a different capacity.
.PHONY: test-capacity
test-capacity:
	$(MAKE) BUILD_DIR=build/capacity3 CPPFLAGS=-DCLEVERBOX_RADIATOR_CAPACITY=3 test test-http

# Optional real TCP broker smoke; not part of deterministic fake-client tests.
.PHONY: test-mqtt-broker
test-mqtt-broker: host
	CLEVERBOX_SIM_LIBRARY=$(abspath $(BUILD_DIR)/host/libcleverbox.so) python3 host/test_mqtt_broker.py

# Optional actual Home Assistant + TCP broker integration (pinned Python deps).
.PHONY: test-home-assistant
test-home-assistant: host
	CLEVERBOX_SIM_LIBRARY=$(abspath $(BUILD_DIR)/host/libcleverbox.so) python3 host/test_home_assistant.py

# Force the actual serializer past its output bound: partial JSON must never publish.
.PHONY: test-discovery-limit
test-discovery-limit:
	$(MAKE) BUILD_DIR=build/discovery-limit CPPFLAGS=-DMQTT_DISCOVERY_PAYLOAD_BYTES=128 host
	CLEVERBOX_DISCOVERY_LIMIT_TEST=1 CLEVERBOX_SIM_LIBRARY=$(abspath build/discovery-limit/host/libcleverbox.so) python3 host/test_mqtt_discovery.py
	CLEVERBOX_DISCOVERY_LIMIT_TEST=1 CLEVERBOX_SIM_LIBRARY=$(abspath build/discovery-limit/host/libcleverbox.so) python3 host/test_passive_ha.py

# Optional real-library register/HAL probes; no ESP-IDF or physical radio.
.PHONY: test-radiolib
test-radiolib:
	@test -n "$(RADIOLIB_SOURCE)" || (echo 'Set RADIOLIB_SOURCE to an exact checkout'; exit 1)
	cmake -S test/radiolib -B $(BUILD_DIR)/radiolib -DRADIOLIB_SOURCE=$(abspath $(RADIOLIB_SOURCE))
	cmake --build $(BUILD_DIR)/radiolib --parallel 4
	./$(BUILD_DIR)/radiolib/test_cc1101

# Compile the production HTTP branch without CLEVERBOX_HOST; only hardware/RF
# adapters remain simulated. This catches physical clock admission regressions.
.PHONY: test-http-physical
test-http-physical: host
	$(CC) $(CPPFLAGS) $(filter-out -DCLEVERBOX_HOST,$(HOST_CFLAGS)) -c firmware/main/http_api.c -o $(BUILD_DIR)/host/http_api_physical.o
	$(CXX) -shared $(filter-out $(BUILD_DIR)/host/firmware/main/http_api.o,$(HOST_OBJECTS)) $(BUILD_DIR)/host/http_api_physical.o $(BUILD_DIR)/host/simulation.o $(shell pkg-config --libs libcjson) -o $(BUILD_DIR)/host/libcleverbox-physical-http.so
	CLEVERBOX_SIM_LIBRARY=$(abspath $(BUILD_DIR)/host/libcleverbox-physical-http.so) python3 host/test_http_boundary.py
