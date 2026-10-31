#pragma once
/* storage - event log + settings.
 *  - Log: every line goes to /sdcard/ACCESS.LOG on the SD card (SPI2: SCK 14,
 *    MOSI 13, MISO 34, CS 4) and to a RAM ring of the last STORAGE_RING lines.
 *    If the card cannot be mounted, only the RAM ring is used.
 *  - Settings: small values in flash (NVS), e.g. opening hours and the PIN hash.
 * All functions are safe to call from several tasks (one mutex). */
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define STORAGE_RING      20      /* how many recent log lines are kept in RAM */
#define STORAGE_LINE_LEN  128     /* longest log line, including "seq," */

/* Init order in app_main: storage_init(), schedule_init(), auth_init(). */
esp_err_t storage_init(void);                 /* call first, before everything else */
bool      storage_sd_mounted(void);           /* true when the SD card is in use */

void      storage_log(const char *line);      /* add one event line "seq,<line>" */
void      storage_log_print(int n);           /* print the last n lines kept in RAM */
void      storage_log_dump_sd(int max_lines); /* print the last lines of the SD log file */

/* Small settings that must survive a reboot (schedule, admin PIN hash). */
esp_err_t storage_kv_set(const char *key, const void *data, size_t len);
esp_err_t storage_kv_get(const char *key, void *data, size_t *len); /* ESP_ERR_NOT_FOUND if never saved */
