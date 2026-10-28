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
	  firmware/components/device_store/device_store.c \
	  firmware/components/device_store/test/test_store.c -o $(BUILD_DIR)/test_store
	./$(BUILD_DIR)/test_store
	$(CC) $(CPPFLAGS) -std=c11 -Wall -Wextra -Werror -pedantic -g \
	  -Ifirmware/components/radiator_protocol/include \
	  firmware/components/radiator_protocol/radiator_protocol.c \
	  firmware/components/radiator_protocol/radiator_control.c \
	  firmware/components/radiator_protocol/test/test_control.c -o $(BUILD_DIR)/test_control
	./$(BUILD_DIR)/test_control
	@for header in RadioLib.h esp_err.h esp_log.h esp_timer.h esp_rom_sys.h \
	  driver/gpio.h driver/spi_master.h freertos/FreeRTOS.h freertos/task.h freertos/queue.h; do \
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
	  -Ifirmware/components/radio_transport/include -Ifirmware/components/device_store/include \
	  -Ifirmware/components/radiator_protocol/include \
	  firmware/components/radio_transport/test/test_admission.cpp \
	  $(BUILD_DIR)/transport_protocol.o $(BUILD_DIR)/transport_pairing.o $(BUILD_DIR)/transport_control.o -o $(BUILD_DIR)/test_admission
	./$(BUILD_DIR)/test_admission
	node firmware/main/test_ui.js

# Host simulation needs cJSON development headers/library (e.g. libcjson-dev).
HOST_CFLAGS = -std=c11 -Wall -Wextra -Werror -pedantic -g -fPIC -MMD -MP -DCLEVERBOX_HOST \
  -Ihost/include -Ifirmware/main -Ifirmware/components/radio_transport \
  -Ifirmware/components/radio_transport/include -Ifirmware/components/device_store/include \
  -Ifirmware/components/radiator_protocol/include $(shell pkg-config --cflags libcjson)
HOST_CSOURCES = firmware/components/radiator_protocol/radiator_protocol.c \
  firmware/components/radiator_protocol/radiator_pairing.c \
  firmware/components/radiator_protocol/radiator_control.c \
  firmware/components/device_store/device_store.c firmware/main/http_api.c host/http.c host/storage.c
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

test-http: host
	CLEVERBOX_SIM_LIBRARY=$(abspath $(BUILD_DIR)/host/libcleverbox.so) python3 host/test_http.py
	CLEVERBOX_SIM_LIBRARY=$(abspath $(BUILD_DIR)/host/libcleverbox.so) python3 host/test_registry.py

-include $(HOST_OBJECTS:.o=.d) $(BUILD_DIR)/host/simulation.d

# Compile and exercise the same production application with a different capacity.
.PHONY: test-capacity
test-capacity:
	$(MAKE) BUILD_DIR=build/capacity3 CPPFLAGS=-DCLEVERBOX_RADIATOR_CAPACITY=3 test test-http
