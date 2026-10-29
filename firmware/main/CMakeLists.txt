idf_component_register(SRCS "main.c" "http_api.c" INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event esp_netif nvs_flash esp_http_server json network_setup radiator_protocol radio_transport
    EMBED_FILES "index.html")
