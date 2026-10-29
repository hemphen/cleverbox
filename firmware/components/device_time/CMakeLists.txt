idf_component_register(SRCS "device_time.c" "time_store.c" "time_setup.c"
    INCLUDE_DIRS "include" REQUIRES esp_timer esp_netif esp_event esp_wifi nvs_flash)
