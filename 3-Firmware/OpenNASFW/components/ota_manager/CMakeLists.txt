idf_component_register(
    SRCS "src/ota_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server
    PRIV_REQUIRES app_update http_server
)
