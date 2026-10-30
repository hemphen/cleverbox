idf_component_register(SRCS "main.c" "http_api.c" "http_access.c" INCLUDE_DIRS "."
    REQUIRES mqtt_bridge esp_wifi esp_event esp_netif nvs_flash esp_http_server json device_time network_setup radiator_protocol radio_transport
    EMBED_FILES "index.html")
