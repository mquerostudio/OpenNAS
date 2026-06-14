#include "hdd_monitor.h"

#include <string.h>

#include "board_pins.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tca9554.h"

static const char *TAG = "hdd_mon";

#define POLL_INTERVAL_MS    20        /* 50 Hz */
#define RING_SAMPLES        50        /* 1 s of samples */
#define ACTIVE_WINDOW_SAMP  25        /* 500 ms of samples used for "active" test */

/*
 * ACT pins from SATA drives are open-collector, pulsed LOW during I/O.
 * So raw_input bit = 0 means "asserted / activity". We normalize to
 * `is_active = (raw_bit == 0)` in software.
 */
#define HDD_RAW_ACTIVE_LEVEL 0

typedef struct {
    uint8_t samples[RING_SAMPLES]; /* 0 = idle, 1 = active */
    int head;                      /* next write position */
    uint32_t event_count;
    uint64_t last_active_us;
    bool currently_active;
    int inactive_count;            /* consecutive idle samples (for hysteresis) */
} hdd_ring_t;

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_tca_dev;
static hdd_ring_t s_rings[HDD_MON_COUNT];
static uint8_t s_active_mask;
static SemaphoreHandle_t s_lock;

/* -------- helpers -------- */

static bool sample_any_active(const hdd_ring_t *r)
{
    /* Look at the latest ACTIVE_WINDOW_SAMP samples ending at (head-1). */
    for (int k = 0; k < ACTIVE_WINDOW_SAMP; ++k) {
        int idx = (r->head - 1 - k + RING_SAMPLES) % RING_SAMPLES;
        if (r->samples[idx]) return true;
    }
    return false;
}

static void update_hdd(int i, bool is_active_sample)
{
    hdd_ring_t *r = &s_rings[i];
    uint8_t prev = r->samples[(r->head - 1 + RING_SAMPLES) % RING_SAMPLES];
    r->samples[r->head] = is_active_sample ? 1 : 0;
    r->head = (r->head + 1) % RING_SAMPLES;

    if (is_active_sample) {
        r->last_active_us = esp_timer_get_time();
        r->inactive_count = 0;
        if (!prev) r->event_count++;   /* idle → active edge */
    } else {
        if (r->inactive_count < RING_SAMPLES) r->inactive_count++;
    }

    /* Hysteresis: become active immediately on any activity in window;
     * only drop to inactive after a full RING_SAMPLES (1 s) of idleness. */
    bool active_now = sample_any_active(r);
    if (active_now) {
        r->currently_active = true;
    } else if (r->inactive_count >= RING_SAMPLES) {
        r->currently_active = false;
    }
}

static void recompute_mask_locked(void)
{
    uint8_t mask = 0;
    for (int i = 0; i < HDD_MON_COUNT; ++i) {
        if (s_rings[i].currently_active) mask |= (1U << i);
    }
    s_active_mask = mask;
}

static void hdd_poll_task(void *arg)
{
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();
    uint32_t log_divider = 0;
    for (;;) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(POLL_INTERVAL_MS));

        uint8_t port = 0;
        if (tca9554_read_inputs(s_tca_dev, &port) != ESP_OK) {
            /* Don't spam logs — one line every ~10 s if it keeps failing. */
            if ((log_divider++ % 500) == 0) ESP_LOGW(TAG, "I2C read failed");
            continue;
        }

        /* First ~5 polls after boot: dump raw byte so polarity can be verified. */
        if (log_divider < 5) {
            ESP_LOGI(TAG, "raw TCA9554 input port = 0x%02X", port);
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int i = 0; i < HDD_MON_COUNT; ++i) {
            int pin = board_hdd_to_pin(i + 1);
            int raw_bit = (port >> pin) & 0x01;
            bool is_active = (raw_bit == HDD_RAW_ACTIVE_LEVEL);
            update_hdd(i, is_active);
        }
        recompute_mask_locked();
        xSemaphoreGive(s_lock);

        log_divider++;
    }
}

/* -------- public API -------- */

esp_err_t hdd_mon_init(void)
{
    memset(s_rings, 0, sizeof(s_rings));
    s_active_mask = 0;

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = BOARD_I2C_PORT_NUM,
        .scl_io_num = BOARD_I2C_SCL_GPIO,
        .sda_io_num = BOARD_I2C_SDA_GPIO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,  /* belt-and-braces; external 4.7k required */
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_bus));

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_TCA9554_I2C_ADDR,
        .scl_speed_hz = BOARD_I2C_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_tca_dev));

    esp_err_t err = tca9554_init(s_tca_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TCA9554 init failed: %s", esp_err_to_name(err));
        return err;
    }

    BaseType_t r = xTaskCreate(hdd_poll_task, "hdd_poll", 2048, NULL, 6, NULL);
    if (r != pdPASS) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "started (I2C SCL=%d SDA=%d addr=0x%02X, %u HDDs @ %u Hz)",
             BOARD_I2C_SCL_GPIO, BOARD_I2C_SDA_GPIO, BOARD_TCA9554_I2C_ADDR,
             HDD_MON_COUNT, 1000 / POLL_INTERVAL_MS);
    return ESP_OK;
}

esp_err_t hdd_mon_get_state(uint8_t hdd_id, hdd_state_t *out)
{
    if (hdd_id < 1 || hdd_id > HDD_MON_COUNT || !out) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    hdd_ring_t *r = &s_rings[hdd_id - 1];
    out->active = r->currently_active;
    out->last_active_us = r->last_active_us;
    out->event_count = r->event_count;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

uint8_t hdd_mon_get_active_mask(void)
{
    uint8_t m;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    m = s_active_mask;
    xSemaphoreGive(s_lock);
    return m;
}
