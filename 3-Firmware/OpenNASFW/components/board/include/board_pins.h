/*
 * OpenNAS board pin map — ESP32-C5-WROOM-1
 * Extracted from KiCad netlist (2-ECAD/OpenNAS.net).
 */
#pragma once

/* Fans (Noctua NF-A8, 4-pin PWM, 25 kHz, 2 pulses/rev tach) */
#define BOARD_FAN0_PWM_GPIO     3   /* → J302 pin 4 */
#define BOARD_FAN0_TACH_GPIO   10   /* ← J302 pin 3, pull-up R304 */
#define BOARD_FAN1_PWM_GPIO     6   /* → J303 pin 4 */
#define BOARD_FAN1_TACH_GPIO    7   /* ← J303 pin 3, pull-up R305 */

/* I2C bus to TCA9554 GPIO expander (U301) */
#define BOARD_I2C_SCL_GPIO      8
#define BOARD_I2C_SDA_GPIO      9
#define BOARD_I2C_PORT_NUM      0
#define BOARD_I2C_FREQ_HZ       400000

/* TCA9554 reset line (active-low; also a strapping pin — drive HIGH after boot) */
#define BOARD_TCA9554_RESET_GPIO 5
#define BOARD_TCA9554_I2C_ADDR   0x20  /* A0=A1=A2=GND (verify on schematic) */

/*
 * HDD activity → TCA9554 port mapping (verified from netlist).
 * HDDs 1-6 are on consecutive ports P0..P5. P6 and P7 are unused.
 */
#define BOARD_HDD_COUNT         6

/* Returns the TCA9554 P-pin (0..7) for a given hdd_id (1..6). */
static inline int board_hdd_to_pin(int hdd_id)
{
    if (hdd_id < 1 || hdd_id > BOARD_HDD_COUNT) return -1;
    return hdd_id - 1;   /* HDD1→P0, HDD2→P1, … HDD6→P5 */
}
