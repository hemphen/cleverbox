idf_component_register(SRCS "network_policy.c" "network_button.c" "network_store.c" "network_setup.c"
    INCLUDE_DIRS "include" REQUIRES esp_wifi esp_event esp_netif esp_timer nvs_flash esp_driver_gpio)
