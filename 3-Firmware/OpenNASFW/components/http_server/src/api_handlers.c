#include "http_server.h"

#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "fan_control.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hdd_monitor.h"
#include "net_manager.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "http_api";

/* ---------------- Auth (Bearer token) ----------------
 * Single source of truth for write-access auth across the whole HTTP API.
 * Effective token: NVS opennas/auth_token if present, else CONFIG_OPENNAS_AUTH_TOKEN.
 * Enforced on POST endpoints only; GET /api/status stays open. */

static const char *effective_token(void)
{
    static char tok[64];
    static bool loaded = false;
    if (loaded) return tok;
    nvs_handle_t h;
    size_t len = sizeof(tok);
    if (nvs_open("opennas", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, "auth_token", tok, &len) != ESP_OK) {
            snprintf(tok, sizeof(tok), "%s", CONFIG_OPENNAS_AUTH_TOKEN);
        }
        nvs_close(h);
    } else {
        snprintf(tok, sizeof(tok), "%s", CONFIG_OPENNAS_AUTH_TOKEN);
    }
    loaded = true;
    return tok;
}

/* Constant-time compare (avoids a timing oracle on the token). */
static bool ct_equal(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    unsigned char diff = (unsigned char)(la ^ lb);
    size_t n = la > lb ? la : lb;
    for (size_t i = 0; i < n; ++i)
        diff |= (unsigned char)((i < la ? a[i] : 0) ^ (i < lb ? b[i] : 0));
    return diff == 0;
}

/* True iff the Authorization header carries a valid "Bearer <token>". */
bool http_auth_ok(httpd_req_t *req)
{
    char hdr[128];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK)
        return false;
    const char *p = hdr;
    if (strncmp(p, "Bearer ", 7) != 0) return false;
    p += 7;
    while (*p == ' ') p++;
    return ct_equal(p, effective_token());
}

/* Embedded dashboard (see CMakeLists.txt EMBED_TXTFILES). */
extern const char dashboard_html_start[] asm("_binary_dashboard_html_start");
extern const char dashboard_html_end[]   asm("_binary_dashboard_html_end");

/* ---------------- GET / (dashboard) ---------------- */

static esp_err_t h_root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, dashboard_html_start,
                           dashboard_html_end - dashboard_html_start);
}

/* ---------------- GET /favicon.ico ----------------
 * Return 204 so browsers stop spamming the log. */
static esp_err_t h_favicon_get(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

/* ---------------- GET /api/status ---------------- */

static esp_err_t h_status_get(httpd_req_t *req)
{
    /* Buffer large enough for all fields; we serialise by hand with snprintf. */
    char buf[768];
    int n = 0;

    n += snprintf(buf + n, sizeof(buf) - n, "{\"fans\":[");
    for (int i = 0; i < FAN_COUNT; ++i) {
        uint8_t duty = 0;
        uint16_t rpm = 0;
        fan_ctrl_get_duty(i, &duty);
        fan_ctrl_get_rpm(i, &rpm);
        n += snprintf(buf + n, sizeof(buf) - n,
                      "%s{\"id\":%d,\"duty\":%u,\"rpm\":%u}",
                      (i == 0) ? "" : ",", i, duty, rpm);
    }

    n += snprintf(buf + n, sizeof(buf) - n, "],\"hdds\":[");
    uint64_t now_us = esp_timer_get_time();
    for (int id = 1; id <= HDD_MON_COUNT; ++id) {
        hdd_state_t st = { 0 };
        hdd_mon_get_state(id, &st);
        uint64_t ago_ms = (st.last_active_us == 0)
                          ? 0 : (now_us - st.last_active_us) / 1000;
        n += snprintf(buf + n, sizeof(buf) - n,
                      "%s{\"id\":%d,\"active\":%s,\"last_ms\":%llu,\"events\":%lu}",
                      (id == 1) ? "" : ",",
                      id, st.active ? "true" : "false",
                      (unsigned long long)ago_ms,
                      (unsigned long)st.event_count);
    }
    n += snprintf(buf + n, sizeof(buf) - n, "]");

    /* System metadata */
    n += snprintf(buf + n, sizeof(buf) - n,
                  ",\"uptime_s\":%llu,\"free_heap\":%lu",
                  (unsigned long long)(now_us / 1000000ULL),
                  (unsigned long)esp_get_free_heap_size());

    /* Fan-control watchdog / failsafe state */
    n += snprintf(buf + n, sizeof(buf) - n,
                  ",\"failsafe\":%s,\"ms_since_cmd\":%lu",
                  fan_ctrl_in_failsafe() ? "true" : "false",
                  (unsigned long)fan_ctrl_ms_since_cmd());

    const esp_app_desc_t *app = esp_app_get_description();
    if (app) {
        n += snprintf(buf + n, sizeof(buf) - n,
                      ",\"version\":\"%s\",\"build_date\":\"%s\"",
                      app->version, app->date);
    }

    esp_ip4_addr_t ip = { 0 };
    if (net_mgr_get_ip(&ip) == ESP_OK) {
        n += snprintf(buf + n, sizeof(buf) - n,
                      ",\"ip\":\"" IPSTR "\"", IP2STR(&ip));
    }

    n += snprintf(buf + n, sizeof(buf) - n, "}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

/* ---------------- POST /api/fan/{0,1} ---------------- */

/* Tiny JSON parser: finds `"key":<num>` and returns the number. Returns -1 if
 * not found, -2 on parse error. Spaces and case-sensitive key matching only. */
static long json_get_int(const char *body, const char *key)
{
    size_t kl = strlen(key);
    for (const char *p = body; (p = strchr(p, '"')) != NULL;) {
        p++;  /* step past the opening quote */
        if (strncmp(p, key, kl) != 0 || p[kl] != '"') continue;
        p = strchr(p + kl + 1, ':');
        if (!p) return -2;
        p++;
        while (*p == ' ' || *p == '\t') p++;
        char *end = NULL;
        long v = strtol(p, &end, 10);
        return (end == p) ? -2 : v;
    }
    return -1;
}

static esp_err_t h_fan_post(httpd_req_t *req)
{
    if (!http_auth_ok(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "missing/invalid token");
        return ESP_FAIL;
    }

    /* Extract fan id from uri — expect "/api/fan/0" or "/api/fan/1". */
    const char *last_slash = strrchr(req->uri, '/');
    if (!last_slash || last_slash[1] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing fan id");
        return ESP_FAIL;
    }
    int fan_id = last_slash[1] - '0';
    if (fan_id < 0 || fan_id >= FAN_COUNT) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "fan id out of range");
        return ESP_FAIL;
    }

    char body[64];
    int received = 0;
    while (received < (int)sizeof(body) - 1) {
        int r = httpd_req_recv(req, body + received, sizeof(body) - 1 - received);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            break;
        }
        received += r;
    }
    body[received] = '\0';

    long duty = json_get_int(body, "duty");
    if (duty == -1) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing \"duty\"");
        return ESP_FAIL;
    }
    if (duty == -2) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }

    if (duty < 0) duty = 0;
    if (duty > 100) duty = 100;

    esp_err_t err = fan_ctrl_set_duty((uint8_t)fan_id, (uint8_t)duty);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }
    fan_ctrl_note_command();   /* refresh the watchdog: the cockpit is alive */

    ESP_LOGI(TAG, "fan%d duty=%ld%%", fan_id, duty);
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ---------------- POST /api/reset_wifi ---------------- */

static esp_err_t h_reset_wifi_post(httpd_req_t *req)
{
    if (!http_auth_ok(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "missing/invalid token");
        return ESP_FAIL;
    }

    esp_err_t err = net_mgr_clear_creds();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, "ok, rebooting into AP mode\n");
    ESP_LOGW(TAG, "wifi creds cleared, rebooting");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return ESP_OK;
}

/* ---------------- Registration ---------------- */

esp_err_t http_server_register_api(httpd_handle_t handle)
{
    static const httpd_uri_t u_root = {
        .uri = "/", .method = HTTP_GET, .handler = h_root_get,
    };
    static const httpd_uri_t u_favicon = {
        .uri = "/favicon.ico", .method = HTTP_GET, .handler = h_favicon_get,
    };
    static const httpd_uri_t u_status = {
        .uri = "/api/status", .method = HTTP_GET, .handler = h_status_get,
    };
    static const httpd_uri_t u_fan = {
        .uri = "/api/fan/?*",  /* matches /api/fan/0 and /api/fan/1 */
        .method = HTTP_POST, .handler = h_fan_post,
    };
    static const httpd_uri_t u_reset = {
        .uri = "/api/reset_wifi", .method = HTTP_POST, .handler = h_reset_wifi_post,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(handle, &u_root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(handle, &u_favicon));
    ESP_ERROR_CHECK(httpd_register_uri_handler(handle, &u_status));
    ESP_ERROR_CHECK(httpd_register_uri_handler(handle, &u_fan));
    ESP_ERROR_CHECK(httpd_register_uri_handler(handle, &u_reset));
    return ESP_OK;
}
