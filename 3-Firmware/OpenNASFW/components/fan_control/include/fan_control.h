#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define FAN_COUNT       2
#define FAN_DUTY_MIN    0
#define FAN_DUTY_MAX    100

/*
 * Bring up LEDC (25 kHz PWM, 10-bit, two channels) and two PCNT units
 * configured for the tach inputs of each fan. Both fans start at the safe
 * failsafe duty (CONFIG_OPENNAS_FAILSAFE_DUTY) and the device boots in
 * failsafe until the first command arrives. Starts the tach sampling task
 * and the failsafe watchdog task.
 */
esp_err_t fan_ctrl_init(void);

/* Set fan duty in percent (0..100). Applied immediately, in RAM only — the
 * external controller (cockpit) is the source of truth, so this does NOT
 * persist to NVS (avoids flash wear from a continuous control loop). */
esp_err_t fan_ctrl_set_duty(uint8_t fan_id, uint8_t percent);

/* Read current duty setpoint in percent. */
esp_err_t fan_ctrl_get_duty(uint8_t fan_id, uint8_t *out_percent);

/* Read most recent tach measurement. Returns 0 if no pulses seen this
 * second (fan stopped or disconnected). */
esp_err_t fan_ctrl_get_rpm(uint8_t fan_id, uint16_t *out_rpm);

/* Refresh the watchdog: called by the POST /api/fan handler on each command. */
void fan_ctrl_note_command(void);

/* True if the watchdog is in failsafe (no recent command). */
bool fan_ctrl_in_failsafe(void);

/* Milliseconds since the last command (UINT32_MAX if there never was one). */
uint32_t fan_ctrl_ms_since_cmd(void);
