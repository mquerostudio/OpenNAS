#include "ota_manager.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "http_server.h"

static const char *TAG = "ota_mgr";

#define OTA_BUF_SIZE     4096
#define HEALTH_DELAY_MS  30000

/* -------- rollback guard -------- */

static void health_check_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(HEALTH_DELAY_MS));
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "new image marked valid (rollback disarmed)");
    } else {
        ESP_LOGW(TAG, "mark_valid: %s", esp_err_to_name(err));
    }
    vTaskDelete(NULL);
}

/* -------- HTTP handler -------- */

static esp_err_t h_ota_post(httpd_req_t *req)
{
    if (!http_auth_ok(req)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "missing/invalid token");
        return ESP_FAIL;
    }

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA partition");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "OTA → partition %s offset=0x%" PRIx32 " size=0x%" PRIx32,
             target->label, target->address, target->size);

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }

    /* Big stack buffer would blow the httpd task; use heap. */
    char *buf = malloc(OTA_BUF_SIZE);
    if (!buf) {
        esp_ota_abort(ota_handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
        return ESP_FAIL;
    }

    int total = 0;
    for (;;) {
        int r = httpd_req_recv(req, buf, OTA_BUF_SIZE);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r < 0) {
            esp_ota_abort(ota_handle);
            free(buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
            return ESP_FAIL;
        }
        if (r == 0) break;

        err = esp_ota_write(ota_handle, buf, r);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ota_write at %d: %s", total, esp_err_to_name(err));
            esp_ota_abort(ota_handle);
            free(buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
            return ESP_FAIL;
        }
        total += r;
        if ((total & 0xFFFF) == 0) {
            ESP_LOGI(TAG, "… %d bytes", total);
        }
    }
    free(buf);

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }
    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_boot_partition: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }

    ESP_LOGW(TAG, "OTA succeeded (%d bytes), rebooting into %s", total, target->label);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "ok, rebooting\n");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return ESP_OK;
}

/* -------- Public API -------- */

esp_err_t ota_manager_init(void)
{
    /* Rollback guard: only if we booted from an OTA slot AND state is PENDING_VERIFY. */
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK
        && state == ESP_OTA_IMG_PENDING_VERIFY) {
        BaseType_t r = xTaskCreate(health_check_task, "ota_health", 2048, NULL, 1, NULL);
        if (r != pdPASS) return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t ota_manager_register(httpd_handle_t handle)
{
    static const httpd_uri_t u_ota = {
        .uri = "/api/ota", .method = HTTP_POST, .handler = h_ota_post,
    };
    return httpd_register_uri_handler(handle, &u_ota);
}
