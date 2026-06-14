#include "fan_control.h"

#include <string.h>

#include "board_pins.h"
#include "driver/ledc.h"
#include "driver/pulse_cnt.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "fan_ctrl";

#define LEDC_TIMER          LEDC_TIMER_0
#define LEDC_MODE           LEDC_LOW_SPEED_MODE
#define LEDC_RES_BITS       LEDC_TIMER_10_BIT
#define LEDC_FREQ_HZ        25000
#define LEDC_DUTY_MAX       ((1 << 10) - 1)   /* 1023 */

#define TACH_WINDOW_MS      1000
#define TACH_GLITCH_NS      1000              /* 1 µs */
#define TACH_PULSES_PER_REV 2                 /* Noctua 4-pin spec */
#define TACH_MAX_COUNT      2000              /* ≥ 24000 RPM headroom */

#define FAILSAFE_DUTY        CONFIG_OPENNAS_FAILSAFE_DUTY
#define FAILSAFE_TIMEOUT_US  ((uint64_t)CONFIG_OPENNAS_FAILSAFE_TIMEOUT_S * 1000000ULL)
#define WATCHDOG_PERIOD_MS   5000

typedef struct {
    int pwm_gpio;
    int tach_gpio;
    ledc_channel_t ledc_ch;
    pcnt_unit_handle_t pcnt_unit;
    uint8_t duty_pct;
    uint16_t last_rpm;
} fan_ctx_t;

static fan_ctx_t s_fans[FAN_COUNT] = {
    { .pwm_gpio = BOARD_FAN0_PWM_GPIO, .tach_gpio = BOARD_FAN0_TACH_GPIO,
      .ledc_ch = LEDC_CHANNEL_0 },
    { .pwm_gpio = BOARD_FAN1_PWM_GPIO, .tach_gpio = BOARD_FAN1_TACH_GPIO,
      .ledc_ch = LEDC_CHANNEL_1 },
};

/* ------------- Watchdog / failsafe ------------- */

static volatile uint64_t s_last_cmd_us = 0;   /* 0 = nunca hubo comando */
static volatile bool     s_in_failsafe = true;

void fan_ctrl_note_command(void)
{
    s_last_cmd_us = esp_timer_get_time();
    s_in_failsafe = false;
}

bool fan_ctrl_in_failsafe(void) { return s_in_failsafe; }

uint32_t fan_ctrl_ms_since_cmd(void)
{
    if (s_last_cmd_us == 0) return UINT32_MAX;
    return (uint32_t)((esp_timer_get_time() - s_last_cmd_us) / 1000ULL);
}

/* ------------- PWM (LEDC) ------------- */

static esp_err_t ledc_setup(void)
{
    ledc_timer_config_t t = {
        .speed_mode = LEDC_MODE,
        .duty_resolution = LEDC_RES_BITS,
        .timer_num = LEDC_TIMER,
        .freq_hz = LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&t);
    if (err != ESP_OK) return err;

    for (int i = 0; i < FAN_COUNT; ++i) {
        ledc_channel_config_t ch = {
            .gpio_num = s_fans[i].pwm_gpio,
            .speed_mode = LEDC_MODE,
            .channel = s_fans[i].ledc_ch,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LEDC_TIMER,
            .duty = 0,
            .hpoint = 0,
        };
        err = ledc_channel_config(&ch);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

static void apply_duty(fan_ctx_t *fan, uint8_t pct)
{
    if (pct > FAN_DUTY_MAX) pct = FAN_DUTY_MAX;
    uint32_t raw = ((uint32_t)pct * LEDC_DUTY_MAX) / 100U;
    ledc_set_duty(LEDC_MODE, fan->ledc_ch, raw);
    ledc_update_duty(LEDC_MODE, fan->ledc_ch);
    fan->duty_pct = pct;
}

/* ------------- Tachometer (PCNT) ------------- */

static esp_err_t pcnt_setup_fan(fan_ctx_t *fan)
{
    pcnt_unit_config_t uc = {
        .high_limit = TACH_MAX_COUNT,
        .low_limit  = -1,
    };
    esp_err_t err = pcnt_new_unit(&uc, &fan->pcnt_unit);
    if (err != ESP_OK) return err;

    pcnt_glitch_filter_config_t gf = { .max_glitch_ns = TACH_GLITCH_NS };
    err = pcnt_unit_set_glitch_filter(fan->pcnt_unit, &gf);
    if (err != ESP_OK) return err;

    pcnt_chan_config_t cc = {
        .edge_gpio_num  = fan->tach_gpio,
        .level_gpio_num = -1,
    };
    pcnt_channel_handle_t ch = NULL;
    err = pcnt_new_channel(fan->pcnt_unit, &cc, &ch);
    if (err != ESP_OK) return err;

    /* Count only rising edges (tach is open-collector, pulls low while turning). */
    err = pcnt_channel_set_edge_action(ch, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                       PCNT_CHANNEL_EDGE_ACTION_HOLD);
    if (err != ESP_OK) return err;

    err = pcnt_unit_enable(fan->pcnt_unit);
    if (err != ESP_OK) return err;
    err = pcnt_unit_clear_count(fan->pcnt_unit);
    if (err != ESP_OK) return err;
    return pcnt_unit_start(fan->pcnt_unit);
}

static void fan_tach_task(void *arg)
{
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(TACH_WINDOW_MS));
        for (int i = 0; i < FAN_COUNT; ++i) {
            int count = 0;
            if (pcnt_unit_get_count(s_fans[i].pcnt_unit, &count) == ESP_OK) {
                pcnt_unit_clear_count(s_fans[i].pcnt_unit);
                if (count < 0) count = 0;
                /* rpm = pulses * (60 s / window) / pulses_per_rev */
                s_fans[i].last_rpm = (uint16_t)((count * 60000UL / TACH_WINDOW_MS)
                                                / TACH_PULSES_PER_REV);
            }
        }
    }
}

/* ------------- Watchdog task -------------
 * Si no llega comando en FAILSAFE_TIMEOUT, fuerza ambos ventiladores al duty
 * seguro. Sale del failsafe en cuanto fan_ctrl_note_command() refresca. */

static void fan_watchdog_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(WATCHDOG_PERIOD_MS));
        uint64_t now = esp_timer_get_time();
        bool expired = (s_last_cmd_us == 0) || (now - s_last_cmd_us > FAILSAFE_TIMEOUT_US);
        if (expired && !s_in_failsafe) {
            for (int i = 0; i < FAN_COUNT; ++i) apply_duty(&s_fans[i], FAILSAFE_DUTY);
            s_in_failsafe = true;
        }
    }
}

/* ------------- Public API ------------- */

esp_err_t fan_ctrl_init(void)
{
    esp_err_t err = ledc_setup();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LEDC setup failed: %s", esp_err_to_name(err));
        return err;
    }

    for (int i = 0; i < FAN_COUNT; ++i) {
        err = pcnt_setup_fan(&s_fans[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "PCNT fan%d setup failed: %s", i, esp_err_to_name(err));
            return err;
        }
        apply_duty(&s_fans[i], FAILSAFE_DUTY);   /* arranca seguro hasta que el cockpit mande */
        ESP_LOGI(TAG, "fan%d PWM=GPIO%d TACH=GPIO%d failsafe_duty=%u%%",
                 i, s_fans[i].pwm_gpio, s_fans[i].tach_gpio, (unsigned)FAILSAFE_DUTY);
    }
    s_in_failsafe = true;
    s_last_cmd_us = 0;

    BaseType_t r1 = xTaskCreate(fan_tach_task, "fan_tach", 2048, NULL, 5, NULL);
    BaseType_t r2 = xTaskCreate(fan_watchdog_task, "fan_wdt", 2560, NULL, 5, NULL);
    return (r1 == pdPASS && r2 == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t fan_ctrl_set_duty(uint8_t fan_id, uint8_t percent)
{
    if (fan_id >= FAN_COUNT) return ESP_ERR_INVALID_ARG;
    if (percent > FAN_DUTY_MAX) percent = FAN_DUTY_MAX;
    apply_duty(&s_fans[fan_id], percent);
    return ESP_OK;   /* ya NO persiste en NVS: el cockpit es la fuente de verdad (evita desgaste) */
}

esp_err_t fan_ctrl_get_duty(uint8_t fan_id, uint8_t *out_percent)
{
    if (fan_id >= FAN_COUNT || !out_percent) return ESP_ERR_INVALID_ARG;
    *out_percent = s_fans[fan_id].duty_pct;
    return ESP_OK;
}

esp_err_t fan_ctrl_get_rpm(uint8_t fan_id, uint16_t *out_rpm)
{
    if (fan_id >= FAN_COUNT || !out_rpm) return ESP_ERR_INVALID_ARG;
    *out_rpm = s_fans[fan_id].last_rpm;
    return ESP_OK;
}
