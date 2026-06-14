#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_netif_ip_addr.h"

typedef enum {
    NET_MODE_UNINIT = 0,
    NET_MODE_AP,    /* provisioning: own SSID, no internet */
    NET_MODE_STA,   /* connected (or trying to) to a WiFi router */
} net_mode_t;

#define NET_AP_SSID_PREFIX  "OpenNAS-setup"
#define NET_AP_IP_STR       "192.168.4.1"

/*
 * Reads wifi_ssid/wifi_pass from NVS. If missing, starts AP mode with
 * an open SSID "OpenNAS-setup-<xx>". If present, starts STA mode and
 * tries to connect indefinitely (backoff handled internally).
 * Call after board_init().
 */
esp_err_t net_mgr_start(void);

/* Persist credentials; next boot will come up in STA. */
esp_err_t net_mgr_save_creds(const char *ssid, const char *pass);

/* Delete credentials; next boot will come up in AP. */
esp_err_t net_mgr_clear_creds(void);

net_mode_t net_mgr_get_mode(void);
bool       net_mgr_is_sta_connected(void);
esp_err_t  net_mgr_get_ip(esp_ip4_addr_t *out);
