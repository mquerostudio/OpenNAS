idf_component_register(
    SRCS "src/board.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio nvs_flash esp_event esp_netif
)
