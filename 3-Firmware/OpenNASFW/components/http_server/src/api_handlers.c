#include "http_server.h"

#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
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

static char s_token[64];
static bool s_token_loaded = false;

static const char *effective_token(void)
{
    if (s_token_loaded) return s_token;
    nvs_handle_t h;
    size_t len = sizeof(s_token);
    if (nvs_open("opennas", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, "auth_token", s_token, &len) != ESP_OK) {
            snprintf(s_token, sizeof(s_token), "%s", CONFIG_OPENNAS_AUTH_TOKEN);
        }
        nvs_close(h);
    } else {
        snprintf(s_token, sizeof(s_token), "%s", CONFIG_OPENNAS_AUTH_TOKEN);
    }
    s_token_loaded = true;
    return s_token;
}

/* Persist a new API token to NVS and refresh the in-RAM cache (takes effect now,
 * no reboot; WiFi creds and other NVS keys are preserved). */
static esp_err_t set_token(const char *tok)
{
    size_t n = strlen(tok);
    if (n == 0 || n >= sizeof(s_token)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open("opennas", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, "auth_token", tok);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;
    snprintf(s_token, sizeof(s_token), "%s", tok);
    s_token_loaded = true;
    return ESP_OK;
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
    const char *tok = effective_token();
    if (tok[0] == '\0') return false;   /* fail-closed if the token is unset */
    return ct_equal(p, tok);
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

    uint8_t mac[6] = { 0 };
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        n += snprintf(buf + n, sizeof(buf) - n,
                      ",\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\"",
                      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
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

/* Extract `"key":"value"` into out (NUL-terminated). Returns true if found.
 * No escape handling — fine for tokens (hex/alphanumeric). */
static bool json_get_str(const char *body, const char *key, char *out, size_t outlen)
{
    size_t kl = strlen(key);
    for (const char *p = body; (p = strchr(p, '"')) != NULL;) {
        p++;
        if (strncmp(p, key, kl) != 0 || p[kl] != '"') continue;
        p = strchr(p + kl + 1, ':');
        if (!p) return false;
        p++;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '"') return false;
        p++;
        size_t i = 0;
        while (*p && *p != '"' && i + 1 < outlen) out[i++] = *p++;
        out[i] = '\0';
        return true;
    }
    return false;
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

/* ---------------- POST /api/set_token ----------------
 * Body {"token":"<new>"}. Authed with the CURRENT token. Persists the new API
 * token to NVS and applies it immediately (keeps WiFi creds). */

static esp_err_t h_set_token_post(httpd_req_t *req)
{
    if (!http_auth_ok(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "missing/invalid token");
        return ESP_FAIL;
    }

    char body[160];
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

    char newtok[64];
    if (!json_get_str(body, "token", newtok, sizeof(newtok))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing \"token\"");
        return ESP_FAIL;
    }

    esp_err_t err = set_token(newtok);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, esp_err_to_name(err));
        return ESP_FAIL;
    }

    ESP_LOGW(TAG, "API token updated via /api/set_token");
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ---------------- POST /api/reboot ----------------
 * Reinicia el ESP. Autenticado. */
static esp_err_t h_reboot_post(httpd_req_t *req)
{
    if (!http_auth_ok(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "missing/invalid token");
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, "ok, rebooting\n");
    ESP_LOGW(TAG, "reboot requested via /api/reboot");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return ESP_OK;
}

/* ---------------- POST /api/set_wifi ----------------
 * Body {"ssid":"..","pass":".."}. Guarda credenciales y reinicia para conectar
 * a la nueva red. Autenticado. pass puede ir vacío (red abierta). */
static esp_err_t h_set_wifi_post(httpd_req_t *req)
{
    if (!http_auth_ok(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "missing/invalid token");
        return ESP_FAIL;
    }
    char body[256];
    int received = 0;
    while (received < (int)sizeof(body) - 1) {
        int r = httpd_req_recv(req, body + received, sizeof(body) - 1 - received);
        if (r <= 0) { if (r == HTTPD_SOCK_ERR_TIMEOUT) continue; break; }
        received += r;
    }
    body[received] = '\0';

    char ssid[33], pass[65];
    if (!json_get_str(body, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing \"ssid\"");
        return ESP_FAIL;
    }
    if (!json_get_str(body, "pass", pass, sizeof(pass))) pass[0] = '\0';

    esp_err_t err = net_mgr_save_creds(ssid, pass);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, "ok, reconnecting to new network\n");
    ESP_LOGW(TAG, "wifi creds updated via /api/set_wifi, rebooting");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
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
    static const httpd_uri_t u_set_token = {
        .uri = "/api/set_token", .method = HTTP_POST, .handler = h_set_token_post,
    };
    static const httpd_uri_t u_reboot = {
        .uri = "/api/reboot", .method = HTTP_POST, .handler = h_reboot_post,
    };
    static const httpd_uri_t u_set_wifi = {
        .uri = "/api/set_wifi", .method = HTTP_POST, .handler = h_set_wifi_post,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(handle, &u_root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(handle, &u_favicon));
    ESP_ERROR_CHECK(httpd_register_uri_handler(handle, &u_status));
    ESP_ERROR_CHECK(httpd_register_uri_handler(handle, &u_fan));
    ESP_ERROR_CHECK(httpd_register_uri_handler(handle, &u_reset));
    ESP_ERROR_CHECK(httpd_register_uri_handler(handle, &u_set_token));
    ESP_ERROR_CHECK(httpd_register_uri_handler(handle, &u_reboot));
    ESP_ERROR_CHECK(httpd_register_uri_handler(handle, &u_set_wifi));
    return ESP_OK;
}
