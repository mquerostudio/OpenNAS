#include "net_manager.h"

#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "net_mgr";

#define NVS_NAMESPACE   "opennas"
#define NVS_KEY_SSID    "wifi_ssid"
#define NVS_KEY_PASS    "wifi_pass"

#define STA_BIT_CONNECTED   BIT0
#define STA_BIT_FAIL        BIT1

static net_mode_t     s_mode = NET_MODE_UNINIT;
static esp_netif_t   *s_netif_sta = NULL;
static esp_netif_t   *s_netif_ap  = NULL;
static EventGroupHandle_t s_sta_evt = NULL;
static esp_ip4_addr_t s_sta_ip = { 0 };
static int            s_sta_retry = 0;

/* -------- NVS helpers -------- */

static esp_err_t nvs_read_str(const char *key, char *buf, size_t buf_len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    size_t required = buf_len;
    err = nvs_get_str(h, key, buf, &required);
    nvs_close(h);
    return err;
}

static esp_err_t nvs_write_str(const char *key, const char *value)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, key, value);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t nvs_delete_key(const char *key)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_erase_key(h, key);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    /* "key not found" is not an error for the caller. */
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    return err;
}

/* -------- Event handlers -------- */

static void on_wifi_event(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    switch (id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            /* Never give up: esp_wifi_connect() itself rate-limits scans,
             * so calling it directly from the handler is fine. */
            s_sta_retry++;
            if ((s_sta_retry % 10) == 1) {
                ESP_LOGW(TAG, "STA disconnected (retry %d)", s_sta_retry);
            }
            xEventGroupClearBits(s_sta_evt, STA_BIT_CONNECTED);
            esp_wifi_connect();
            break;
        case WIFI_EVENT_AP_STACONNECTED:
            ESP_LOGI(TAG, "AP: client joined");
            break;
        case WIFI_EVENT_AP_STADISCONNECTED:
            ESP_LOGI(TAG, "AP: client left");
            break;
        default: break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base,
                        int32_t id, void *data)
{
    (void)arg; (void)base;
    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = data;
        s_sta_ip = evt->ip_info.ip;
        s_sta_retry = 0;
        xEventGroupSetBits(s_sta_evt, STA_BIT_CONNECTED);
        ESP_LOGI(TAG, "STA got IP: " IPSTR, IP2STR(&s_sta_ip));
    }
}

/* -------- Mode starters -------- */

static esp_err_t start_ap(void)
{
    s_netif_ap = esp_netif_create_default_wifi_ap();

    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);

    wifi_config_t cfg = { 0 };
    snprintf((char *)cfg.ap.ssid, sizeof(cfg.ap.ssid),
             "%s-%02X%02X", NET_AP_SSID_PREFIX, mac[4], mac[5]);
    cfg.ap.ssid_len = strlen((char *)cfg.ap.ssid);
    cfg.ap.channel = 6;
    cfg.ap.max_connection = 4;
    cfg.ap.authmode = WIFI_AUTH_OPEN;    /* open portal for onboarding */
    cfg.ap.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP started: SSID=%s (open)", (char *)cfg.ap.ssid);
    s_mode = NET_MODE_AP;
    return ESP_OK;
}

static esp_err_t start_sta(const char *ssid, const char *pass)
{
    s_netif_sta = esp_netif_create_default_wifi_sta();

    wifi_config_t cfg = { 0 };
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;  /* accept any; AP decides */
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "STA started, connecting to \"%s\"", ssid);
    s_mode = NET_MODE_STA;
    return ESP_OK;
}

/* -------- Public API -------- */

esp_err_t net_mgr_start(void)
{
    s_sta_evt = xEventGroupCreate();
    if (!s_sta_evt) return ESP_ERR_NO_MEM;

    wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wc));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL, NULL));

    char ssid[33] = { 0 };
    char pass[65] = { 0 };
    esp_err_t r_ssid = nvs_read_str(NVS_KEY_SSID, ssid, sizeof(ssid));
    esp_err_t r_pass = nvs_read_str(NVS_KEY_PASS, pass, sizeof(pass));

    if (r_ssid == ESP_OK && ssid[0] != '\0') {
        if (r_pass != ESP_OK) pass[0] = '\0';
        return start_sta(ssid, pass);
    }
    return start_ap();
}

esp_err_t net_mgr_save_creds(const char *ssid, const char *pass)
{
    if (!ssid || !pass) return ESP_ERR_INVALID_ARG;
    esp_err_t err = nvs_write_str(NVS_KEY_SSID, ssid);
    if (err != ESP_OK) return err;
    return nvs_write_str(NVS_KEY_PASS, pass);
}

esp_err_t net_mgr_clear_creds(void)
{
    esp_err_t e1 = nvs_delete_key(NVS_KEY_SSID);
    esp_err_t e2 = nvs_delete_key(NVS_KEY_PASS);
    return (e1 != ESP_OK) ? e1 : e2;
}

net_mode_t net_mgr_get_mode(void) { return s_mode; }

bool net_mgr_is_sta_connected(void)
{
    if (!s_sta_evt) return false;
    return (xEventGroupGetBits(s_sta_evt) & STA_BIT_CONNECTED) != 0;
}

esp_err_t net_mgr_get_ip(esp_ip4_addr_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (s_mode == NET_MODE_STA) {
        *out = s_sta_ip;
        return ESP_OK;
    }
    if (s_mode == NET_MODE_AP && s_netif_ap) {
        esp_netif_ip_info_t info;
        esp_err_t err = esp_netif_get_ip_info(s_netif_ap, &info);
        if (err == ESP_OK) *out = info.ip;
        return err;
    }
    return ESP_ERR_INVALID_STATE;
}
