#include "http_server.h"

#include "esp_log.h"

static const char *TAG = "httpd";

esp_err_t http_server_start(httpd_handle_t *out_handle)
{
    if (!out_handle) return ESP_ERR_INVALID_ARG;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.ctrl_port = 32768;
    cfg.max_uri_handlers = 16;
    cfg.max_open_sockets = 5;
    cfg.recv_wait_timeout = 30;   /* big OTA uploads */
    cfg.send_wait_timeout = 10;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;  /* needed by /api/fan/?* */
    /* 404 handler is registered by the provisioning component when needed. */

    httpd_handle_t h = NULL;
    esp_err_t err = httpd_start(&h, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }
    *out_handle = h;
    ESP_LOGI(TAG, "http server listening on :80");
    return ESP_OK;
}
