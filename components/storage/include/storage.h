#pragma once
/* storage - event log in RAM (last STORAGE_RING lines) + settings in flash (NVS).
 * NVS is used instead of an SD card (allowed by the project guide). */
#include <stddef.h>
#include "esp_err.h"

#define STORAGE_RING  20      /* how many recent log lines are kept */

esp_err_t storage_init(void);                 /* call first, before everything else */
void      storage_log(const char *line);      /* add one event line                 */
void      storage_log_print(void);            /* print the recent lines             */

/* Small settings that must survive a reboot (schedule, admin PIN). */
esp_err_t storage_kv_set(const char *key, const void *data, size_t len);
esp_err_t storage_kv_get(const char *key, void *data, size_t *len);
