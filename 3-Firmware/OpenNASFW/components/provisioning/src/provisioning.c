#include "provisioning.h"

#include <string.h>

#include "dns_hijack.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net_manager.h"

static const char *TAG = "provision";

/* Minimal HTML, inlined. Keep it tiny — one page only. */
static const char INDEX_HTML[] =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>OpenNAS Setup</title>"
    "<style>body{font-family:system-ui,sans-serif;max-width:420px;margin:2em auto;"
    "padding:0 1em;color:#222}h1{font-size:1.4em}label{display:block;margin-top:1em;"
    "font-weight:600}input{width:100%;padding:.6em;font-size:1em;border:1px solid #888;"
    "border-radius:6px;box-sizing:border-box}button{margin-top:1.5em;padding:.7em 1.2em;"
    "font-size:1em;background:#0066cc;color:#fff;border:0;border-radius:6px;"
    "cursor:pointer;width:100%}</style></head><body>"
    "<h1>OpenNAS WiFi setup</h1>"
    "<p>Enter the credentials of your WiFi network. The device will reboot and connect.</p>"
    "<form method=\"POST\" action=\"/provision\">"
    "<label>Network name (SSID)</label><input name=\"ssid\" maxlength=\"32\" required>"
    "<label>Password</label><input name=\"pass\" type=\"password\" maxlength=\"64\">"
    "<button type=\"submit\">Save and reboot</button></form></body></html>";

static const char SAVED_HTML[] =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<title>Saved</title></head><body style=\"font-family:system-ui;max-width:420px;"
    "margin:2em auto;padding:0 1em\">"
    "<h1>Saved.</h1><p>Rebooting now. The device will try to connect to your WiFi. "
    "When it joins your network, find it under the hostname <code>opennas</code> or "
    "check your router's DHCP leases for its IP.</p></body></html>";

/* -------- URL-encoded form parsing -------- */

static int hex_nybble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

/* Decodes application/x-www-form-urlencoded into a buffer in place.
 * Returns decoded length or -1. */
static int urldecode(char *s, int n)
{
    int w = 0;
    for (int r = 0; r < n; ) {
        char c = s[r];
        if (c == '+') { s[w++] = ' '; r++; }
        else if (c == '%' && r + 2 < n) {
            int hi = hex_nybble(s[r+1]), lo = hex_nybble(s[r+2]);
            if (hi < 0 || lo < 0) return -1;
            s[w++] = (char)((hi << 4) | lo);
            r += 3;
        } else { s[w++] = c; r++; }
    }
    s[w] = '\0';
    return w;
}

/* Extracts value of `key` from body into `out` (size out_max). NUL-terminated.
 * Returns 0 on success, -1 if key not present. */
static int form_get(char *body, const char *key, char *out, int out_max)
{
    int kl = strlen(key);
    for (char *p = body; *p;) {
        char *eq = strchr(p, '=');
        if (!eq) return -1;
        char *amp = strchr(eq + 1, '&');
        int vstart = eq - body + 1;
        int vend = amp ? (amp - body) : (int)strlen(body);
        int keylen = eq - p;
        if (keylen == kl && memcmp(p, key, kl) == 0) {
            int vlen = vend - vstart;
            if (vlen >= out_max) vlen = out_max - 1;
            memcpy(out, body + vstart, vlen);
            out[vlen] = '\0';
            int dec = urldecode(out, vlen);
            return (dec < 0) ? -1 : 0;
        }
        if (!amp) break;
        p = amp + 1;
    }
    return -1;
}

/* -------- HTTP handlers -------- */

static esp_err_t h_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, INDEX_HTML, sizeof(INDEX_HTML) - 1);
    return ESP_OK;
}

static esp_err_t h_provision_post(httpd_req_t *req)
{
    char body[256];
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

    char ssid[33] = { 0 };
    char pass[65] = { 0 };
    if (form_get(body, "ssid", ssid, sizeof(ssid)) != 0 || ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing SSID");
        return ESP_FAIL;
    }
    form_get(body, "pass", pass, sizeof(pass));  /* empty pass is valid */

    esp_err_t err = net_mgr_save_creds(ssid, pass);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "saved creds for SSID \"%s\", rebooting", ssid);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, SAVED_HTML, sizeof(SAVED_HTML) - 1);

    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

/* 404 → 302 to the portal. Captures Apple/Android/Windows captive probes. */
static esp_err_t h_captive_404(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* -------- Public API -------- */

esp_err_t provisioning_start(httpd_handle_t handle)
{
    static const httpd_uri_t u_root = {
        .uri = "/", .method = HTTP_GET, .handler = h_index,
    };
    static const httpd_uri_t u_provision_get = {
        .uri = "/provision", .method = HTTP_GET, .handler = h_index,
    };
    static const httpd_uri_t u_provision_post = {
        .uri = "/provision", .method = HTTP_POST, .handler = h_provision_post,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(handle, &u_root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(handle, &u_provision_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(handle, &u_provision_post));
    ESP_ERROR_CHECK(httpd_register_err_handler(handle, HTTPD_404_NOT_FOUND, h_captive_404));

    /* Get AP IP and start DNS hijack */
    esp_ip4_addr_t ip = { 0 };
    esp_err_t err = net_mgr_get_ip(&ip);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "net_mgr_get_ip failed, defaulting to 192.168.4.1");
        ip.addr = 0x0104A8C0;  /* 192.168.4.1 in network byte order */
    }
    return dns_hijack_start(ip.addr);
}

void provisioning_stop(void)
{
    dns_hijack_stop();
}
