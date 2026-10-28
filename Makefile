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
