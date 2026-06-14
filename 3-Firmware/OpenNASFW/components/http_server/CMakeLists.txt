idf_component_register(
    SRCS "src/http_server.c" "src/api_handlers.c"
    INCLUDE_DIRS "include"
    EMBED_TXTFILES "assets/dashboard.html"
    REQUIRES esp_http_server
    PRIV_REQUIRES fan_control hdd_monitor net_manager esp_app_format esp_hw_support nvs_flash esp_wifi
)
