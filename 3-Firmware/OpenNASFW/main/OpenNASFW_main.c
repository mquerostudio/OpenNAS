/*
 * OpenNAS firmware entry point — boot orchestrator.
 *
 * Init order matters:
 *   1. board:   NVS, event loop, netif, TCA9554 reset release
 *   2. fans:    LEDC + PCNT, load persisted setpoints
 *   3. hdds:    I2C bus + TCA9554 + poll task
 *   4. net:     WiFi (AP provisioning, or STA if creds saved)
 *   5. httpd:   one shared server
 *   6. http endpoints: captive portal (AP) or REST+OTA (STA)
 *   7. ota:     rollback guard if booted from an OTA slot
 */

#include "board.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "fan_control.h"
#include "hdd_monitor.h"
#include "http_server.h"
#include "net_manager.h"
#include "ota_manager.h"
#include "provisioning.h"

static const char *TAG = "main";

void app_main(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "OpenNAS fw %s built %s %s",
             app ? app->version : "?",
             app ? app->date    : "?",
             app ? app->time    : "?");

    ESP_ERROR_CHECK(board_init());
    ESP_ERROR_CHECK(fan_ctrl_init());
    ESP_ERROR_CHECK(hdd_mon_init());
    ESP_ERROR_CHECK(net_mgr_start());
    ESP_ERROR_CHECK(ota_manager_init());

    httpd_handle_t httpd = NULL;
    ESP_ERROR_CHECK(http_server_start(&httpd));

    if (net_mgr_get_mode() == NET_MODE_AP) {
        ESP_LOGW(TAG, "no wifi creds — starting captive portal");
        ESP_ERROR_CHECK(provisioning_start(httpd));
    } else {
        ESP_LOGI(TAG, "STA mode — registering REST API and OTA endpoint");
        ESP_ERROR_CHECK(http_server_register_api(httpd));
        ESP_ERROR_CHECK(ota_manager_register(httpd));
    }

    ESP_LOGI(TAG, "boot complete");
}
