#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/*
 * One-time init:
 *   - If running from an OTA slot, arm a deferred `esp_ota_mark_app_valid`
 *     after 30 s of uptime (rollback guard).
 *
 * Must be called after board_init().
 */
esp_err_t ota_manager_init(void);

/* Register POST /api/ota on an already-started httpd handle (auth via the
 * shared http_auth_ok / API token). Should be called ONLY when in STA mode. */
esp_err_t ota_manager_register(httpd_handle_t handle);
