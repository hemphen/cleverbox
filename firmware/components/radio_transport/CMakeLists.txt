idf_component_register(SRCS "radio_transport.cpp" INCLUDE_DIRS "include"
    REQUIRES radiator_protocol RadioLib esp_driver_gpio esp_driver_spi esp_timer)
