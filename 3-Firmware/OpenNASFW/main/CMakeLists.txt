idf_component_register(
    SRCS "OpenNASFW_main.c"
    INCLUDE_DIRS ""
    PRIV_REQUIRES
        board
        fan_control
        hdd_monitor
        net_manager
        http_server
        provisioning
        ota_manager
        esp_app_format
)
