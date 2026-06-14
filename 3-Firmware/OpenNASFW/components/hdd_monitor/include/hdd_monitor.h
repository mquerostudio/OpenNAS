#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define HDD_MON_COUNT 6   /* hdd_id 1..6 */

typedef struct {
    bool active;               /* sustained activity detected in last 500 ms */
    uint64_t last_active_us;   /* esp_timer_get_time when last saw an active sample */
    uint32_t event_count;      /* monotonic counter of LOW→HIGH transitions seen */
} hdd_state_t;

/* Bring up I2C bus + TCA9554, start polling task (50 Hz). */
esp_err_t hdd_mon_init(void);

/* Snapshot the state of a single HDD (hdd_id is 1..6). */
esp_err_t hdd_mon_get_state(uint8_t hdd_id, hdd_state_t *out);

/* Bitmask of currently-active HDDs (bit 0 = HDD1, bit 5 = HDD6). */
uint8_t hdd_mon_get_active_mask(void);
