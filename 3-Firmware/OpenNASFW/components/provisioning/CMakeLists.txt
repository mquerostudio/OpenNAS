idf_component_register(
    SRCS "src/provisioning.c" "src/dns_hijack.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES esp_http_server
    PRIV_REQUIRES net_manager lwip esp_netif
)
