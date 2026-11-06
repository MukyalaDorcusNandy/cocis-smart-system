#pragma once
/* lcd_display - 16x2 LCD at the entrance (I2C, PCF8574 backpack, address 0x27).
 *
 * Idle screen:        CoCIS  08:05:12
 *                     In:2   OPEN
 * After every scan, for 3 s:
 *                     WELCOME Brian        BYE Alice         DENIED
 *                     Student BLUE card    Staff GREEN card  Building closed
 *
 * Only the lcd task talks to the LCD. Scan decisions reach it through a queue
 * (entrance event listener), so the entrance never waits for the slow I2C bus.
 * If no LCD answers on I2C the system keeps running without it.
 *
 * Call lcd_display_start() BEFORE entrance_start() (it adds an event listener). */
#include "esp_err.h"

esp_err_t lcd_display_start(void);
