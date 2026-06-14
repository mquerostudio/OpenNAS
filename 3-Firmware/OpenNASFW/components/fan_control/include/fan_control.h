#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define FAN_COUNT       2
#define FAN_DUTY_MIN    0
#define FAN_DUTY_MAX    100

/*
 * Bring up LEDC (25 kHz PWM, 10-bit, two channels) and two PCNT units
 * configured for the tach inputs of each fan. Loads persisted duty
 * setpoints from NVS (default 40%) and applies them. Starts the tach
 * sampling task.
 */
esp_err_t fan_ctrl_init(void);

/* Set fan duty in percent (0..100). Persists to NVS. */
esp_err_t fan_ctrl_set_duty(uint8_t fan_id, uint8_t percent);

/* Read current duty setpoint in percent. */
esp_err_t fan_ctrl_get_duty(uint8_t fan_id, uint8_t *out_percent);

/* Read most recent tach measurement. Returns 0 if no pulses seen this
 * second (fan stopped or disconnected). */
esp_err_t fan_ctrl_get_rpm(uint8_t fan_id, uint16_t *out_rpm);

/* Refresca el watchdog: lo llama el handler de POST /api/fan en cada comando. */
void fan_ctrl_note_command(void);

/* true si el watchdog esta en failsafe (sin comando reciente). */
bool fan_ctrl_in_failsafe(void);

/* Milisegundos desde el ultimo comando (UINT32_MAX si nunca hubo). */
uint32_t fan_ctrl_ms_since_cmd(void);
