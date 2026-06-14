#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_http_server.h"

/*
 * Start a single httpd instance with enough URI-handler slots for all
 * components. Call once, after net_mgr_start(). The handle is returned
 * so specialised components (provisioning, ota_manager, REST API) can
 * register their own URIs on it.
 */
esp_err_t http_server_start(httpd_handle_t *out_handle);

/*
 * Register the REST API endpoints (status, fan control, reset_wifi).
 * Should be called ONLY when in STA mode.
 */
esp_err_t http_server_register_api(httpd_handle_t handle);

/*
 * Shared auth gate for control endpoints (fan, reset_wifi, ota). Returns true
 * iff the request carries "Authorization: Bearer <token>" matching the API
 * token (NVS opennas/auth_token, default CONFIG_OPENNAS_AUTH_TOKEN). The single
 * source of truth for write-access auth across the whole HTTP API.
 */
bool http_auth_ok(httpd_req_t *req);
