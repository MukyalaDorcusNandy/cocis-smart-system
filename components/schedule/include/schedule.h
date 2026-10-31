#pragma once
/* schedule - building opening hours (saved in NVS) and the simulated clock.
 * This is the only clock and the only set of opening hours in the system.
 *
 * Init order in app_main: storage_init(), schedule_init(), auth_init().
 *
 * Default hours are 08:00 - 18:00 (group decision; the coursework does not fix them).
 * Opening must be earlier than closing: overnight opening is not supported.
 * The simulated clock starts at 08:00:00 at boot and runs in real time
 * (Wokwi has no RTC); change it with clock_set(). */
#include <stdbool.h>
#include "esp_err.h"

esp_err_t   schedule_init(void);
esp_err_t   schedule_set(int open_min, int close_min);  /* minutes after midnight */
void        schedule_get(int *open_min, int *close_min);
bool        schedule_is_open(void);

esp_err_t   clock_set(int hour, int minute);             /* hour 0-23, minute 0-59 */
int         clock_minutes(void);
const char *clock_str(char *buf, int len);               /* "HH:MM:SS" */

/* Stores one event line "HH:MM:SS,<event>" through storage_log(). */
void        schedule_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
