#pragma once
/* schedule - simulated clock and access hours (stored in NVS).
 * Default hours: 08:00 - 18:00 (as specified in project requirements). */
#include <stdbool.h>
#include "esp_err.h"

esp_err_t   schedule_init(void);
esp_err_t   schedule_set(int open_min, int close_min);
void        schedule_get(int *open_min, int *close_min);
bool        schedule_is_open(void);

void        clock_set(int hour, int minute);
int         clock_minutes(void);
const char *clock_str(char *buf, int len);

/* Logs an event prefixed with simulated time: "HH:MM:SS,<event>" */
void        schedule_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
