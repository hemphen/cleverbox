idf_component_register(SRCS "network_setup.c" "network_wifi.c"
    INCLUDE_DIRS "include" REQUIRES device_store esp_wifi esp_event esp_netif esp_timer esp_driver_gpio)
