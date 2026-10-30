#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "storage.h"

/* Fixed ring buffer in RAM: keeps the most recent 20 event log lines */
static char              s_log[STORAGE_RING][80];

/* Total count of log lines recorded since system boot (used as sequence ID) */
static int               s_count;

/* Mutex lock preventing concurrent access from multiple FreeRTOS tasks */
static SemaphoreHandle_t s_lock;

/**
 * Initializes the storage subsystem:
 * 1. Creates the mutex lock.
 * 2. Initializes ESP32 Non-Volatile Storage (NVS) in flash memory.
 *    If NVS partition is corrupted or unformatted, erases and re-initializes.
 */
esp_err_t storage_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        e = nvs_flash_init();
    }
    printf("[STORAGE] settings in NVS %s, log in RAM (last %d lines)\n",
           e == ESP_OK ? "OK" : "FAILED", STORAGE_RING);
    return e;
}

/**
 * Appends a log line into the circular RAM ring buffer.
 * Formats each entry with an incrementing sequence number: "<seq>,<line>".
 * Once full, automatically overwrites the oldest line (modulo index).
 */
void storage_log(const char *line)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_count++;
    snprintf(s_log[s_count % STORAGE_RING], sizeof(s_log[0]), "%d,%s", s_count, line);
    xSemaphoreGive(s_lock);
}

/**
 * Prints all available logged events in chronological order to the serial console.
 */
void storage_log_print(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* Calculate start index: either from 1 or from (s_count - 19) */
    int first = s_count > STORAGE_RING ? s_count - STORAGE_RING + 1 : 1;
    for (int i = first; i <= s_count; i++) {
        printf("%s\n", s_log[i % STORAGE_RING]);
    }
    if (s_count == 0) {
        printf("(log is empty)\n");
    }
    xSemaphoreGive(s_lock);
}

/**
 * Writes an arbitrary data blob into flash NVS under the "cocis" namespace.
 * Used to persist settings like schedule hours and admin PIN.
 */
esp_err_t storage_kv_set(const char *key, const void *data, size_t len)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open("cocis", NVS_READWRITE, &h);
    if (e != ESP_OK) return e;

    e = nvs_set_blob(h, key, data, len);
    if (e == ESP_OK) {
        e = nvs_commit(h); /* Write changes to physical flash */
    }
    nvs_close(h);
    return e;
}

/**
 * Reads a stored data blob from flash NVS under the "cocis" namespace.
 */
esp_err_t storage_kv_get(const char *key, void *data, size_t *len)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open("cocis", NVS_READONLY, &h);
    if (e != ESP_OK) return e;

    e = nvs_get_blob(h, key, data, len);
    nvs_close(h);
    return e;
}
