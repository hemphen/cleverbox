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
