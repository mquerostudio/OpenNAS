#pragma once

#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

/*
 * Minimal TCA9554 driver — just what hdd_monitor needs:
 * register-level access via the IDF bus-device I2C API.
 *
 * Register map (TI TCA9554 datasheet):
 *   0x00  Input Port       (R)
 *   0x01  Output Port      (R/W)
 *   0x02  Polarity Inv.    (R/W)
 *   0x03  Configuration    (R/W; 1 = input, 0 = output)
 */

#define TCA9554_REG_INPUT   0x00
#define TCA9554_REG_OUTPUT  0x01
#define TCA9554_REG_POLINV  0x02
#define TCA9554_REG_CONFIG  0x03

esp_err_t tca9554_init(i2c_master_dev_handle_t dev);
esp_err_t tca9554_read_inputs(i2c_master_dev_handle_t dev, uint8_t *out_byte);
esp_err_t tca9554_write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val);
