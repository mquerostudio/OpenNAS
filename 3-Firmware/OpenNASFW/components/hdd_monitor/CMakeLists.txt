idf_component_register(
    SRCS "src/hdd_monitor.c" "src/tca9554.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES esp_driver_i2c esp_timer
    PRIV_REQUIRES board
)
