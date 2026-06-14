#include "tca9554.h"

#include "freertos/FreeRTOS.h"

esp_err_t tca9554_write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(dev, buf, sizeof(buf), pdMS_TO_TICKS(50));
}

esp_err_t tca9554_read_inputs(i2c_master_dev_handle_t dev, uint8_t *out_byte)
{
    const uint8_t reg = TCA9554_REG_INPUT;
    return i2c_master_transmit_receive(dev, &reg, 1, out_byte, 1, pdMS_TO_TICKS(50));
}

esp_err_t tca9554_init(i2c_master_dev_handle_t dev)
{
    /* All ports as inputs. */
    esp_err_t err = tca9554_write_reg(dev, TCA9554_REG_CONFIG, 0xFF);
    if (err != ESP_OK) return err;
    /* No hardware polarity inversion — we handle it in the poll loop. */
    return tca9554_write_reg(dev, TCA9554_REG_POLINV, 0x00);
}
