idf_component_register(SRCS "mqtt_bridge.c" "mqtt_discovery.c" "mqtt_store.c" "mqtt_setup.c" INCLUDE_DIRS "include"
    REQUIRES mqtt json nvs_flash esp_timer esp_hw_support esp_netif network_setup radio_transport)
