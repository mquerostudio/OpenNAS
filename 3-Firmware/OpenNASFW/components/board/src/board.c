#include "board.h"

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "board";

esp_err_t board_init(void)
{
    /* NVS — reformat on version mismatch / corruption. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS corrupted or new version; erasing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Release TCA9554 reset (active-low). GPIO5 is strapping on C5 — ok to
     * drive after boot, the PCB must keep it in the right state at reset. */
    gpio_config_t rst_cfg = {
        .pin_bit_mask = 1ULL << BOARD_TCA9554_RESET_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&rst_cfg));
    gpio_set_level(BOARD_TCA9554_RESET_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(2));  /* datasheet: tRR ≥ 600 ns, 2 ms is overkill-safe */

    ESP_LOGI(TAG, "board ready");
    return ESP_OK;
}
