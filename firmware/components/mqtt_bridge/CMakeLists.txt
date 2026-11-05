idf_component_register(SRCS "mqtt_bridge.c" "mqtt_client.c" INCLUDE_DIRS "include"
    REQUIRES mqtt json esp_timer esp_hw_support device_store device_time radio_transport)
