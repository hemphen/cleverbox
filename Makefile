.PHONY: test
test:
	@mkdir -p build
	$(CC) -std=c11 -Wall -Wextra -Werror -pedantic -g \
	  -Ifirmware/components/radiator_protocol/include \
	  firmware/components/radiator_protocol/radiator_protocol.c \
	  firmware/components/radiator_protocol/test/test_protocol.c -o build/test_protocol
	./build/test_protocol
	$(CC) -std=c11 -Wall -Wextra -Werror -pedantic -g \
	  -Ifirmware/components/radiator_protocol/include \
	  firmware/components/radiator_protocol/radiator_protocol.c \
	  firmware/components/radiator_protocol/radiator_pairing.c \
	  firmware/components/radiator_protocol/test/test_pairing.c -o build/test_pairing
	./build/test_pairing
	$(CC) -std=c11 -Wall -Wextra -Werror -pedantic -g \
	  -Ifirmware/components/device_store/include \
	  -Ifirmware/components/device_store/test/stubs \
	  firmware/components/device_store/device_store.c \
	  firmware/components/device_store/test/test_store.c -o build/test_store
	./build/test_store
	$(CC) -std=c11 -Wall -Wextra -Werror -pedantic -g \
	  -Ifirmware/components/radiator_protocol/include \
	  firmware/components/radiator_protocol/radiator_protocol.c \
	  firmware/components/radiator_protocol/radiator_control.c \
	  firmware/components/radiator_protocol/test/test_control.c -o build/test_control
	./build/test_control
	@for header in RadioLib.h esp_err.h esp_log.h esp_timer.h esp_rom_sys.h \
	  driver/gpio.h driver/spi_master.h freertos/FreeRTOS.h freertos/task.h freertos/queue.h; do \
	  mkdir -p build/transport_stubs/$$(dirname $$header); \
	  printf '#include "transport_stubs.h"\n' > build/transport_stubs/$$header; done
	$(CC) -std=c11 -Ifirmware/components/radiator_protocol/include \
	  -c firmware/components/radiator_protocol/radiator_protocol.c -o build/transport_protocol.o
	$(CC) -std=c11 -Ifirmware/components/radiator_protocol/include \
	  -c firmware/components/radiator_protocol/radiator_pairing.c -o build/transport_pairing.o
	$(CC) -std=c11 -Ifirmware/components/radiator_protocol/include \
	  -c firmware/components/radiator_protocol/radiator_control.c -o build/transport_control.o
	$(CXX) -std=c++17 -Wall -Wextra -Werror -pedantic -g \
	  -Ibuild/transport_stubs -Ifirmware/components/radio_transport/test/stubs \
	  -Ifirmware/components/radio_transport/include -Ifirmware/components/device_store/include \
	  -Ifirmware/components/radiator_protocol/include \
	  firmware/components/radio_transport/test/test_admission.cpp \
	  build/transport_protocol.o build/transport_pairing.o build/transport_control.o -o build/test_admission
	./build/test_admission
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
HOST_OBJECTS = $(patsubst %.c,build/host/%.o,$(HOST_CSOURCES))

build/host/%.o: %.c $(wildcard host/include/*.h) firmware/main/http_api.h
	@mkdir -p $(@D)
	$(CC) $(HOST_CFLAGS) -c $< -o $@

build/host/simulation.o: host/simulation.cpp host/include/platform.h host/include/simulation.h \
  firmware/components/radio_transport/radio_transport.cpp
	@mkdir -p $(@D)
	$(CXX) $(filter-out -std=c11,$(HOST_CFLAGS)) -std=c++17 -c $< -o $@

build/host/libcleverbox.so: $(HOST_OBJECTS) build/host/simulation.o
	$(CXX) -shared $^ $(shell pkg-config --libs libcjson) -o $@

.PHONY: host simulate test-http
host: build/host/libcleverbox.so
simulate: host
	python3 host/serve.py

test-http: host
	python3 host/test_http.py

-include $(HOST_OBJECTS:.o=.d) build/host/simulation.d
