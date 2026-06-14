idf_component_register(
    SRCS "src/net_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_netif esp_wifi esp_event
    PRIV_REQUIRES nvs_flash lwip esp_eth
)
