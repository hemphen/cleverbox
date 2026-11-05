idf_component_register(SRCS "device_time.c" "time_sntp.c"
    INCLUDE_DIRS "include" REQUIRES device_store esp_timer esp_netif esp_event)
