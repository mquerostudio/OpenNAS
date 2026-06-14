#pragma once

#include "esp_err.h"
#include "board_pins.h"

/*
 * Bring the board to a known state:
 *   - NVS flash init (erases + reinits if corrupted)
 *   - Default event loop + netif
 *   - Release the TCA9554 RESET line (GPIO5 HIGH)
 *
 * Call first in app_main, before any other component init.
 */
esp_err_t board_init(void);
