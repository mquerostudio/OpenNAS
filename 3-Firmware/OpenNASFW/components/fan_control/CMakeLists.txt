idf_component_register(
    SRCS "src/fan_control.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_ledc esp_driver_pcnt esp_timer
    PRIV_REQUIRES board
)
