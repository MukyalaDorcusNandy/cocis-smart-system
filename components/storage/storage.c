#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "storage.h"

/* SD card wiring (must match diagram.json) */
#define PIN_SD_SCK    14
#define PIN_SD_MOSI   13
#define PIN_SD_MISO   34
#define PIN_SD_CS     4

#define MOUNT_POINT   "/sdcard"
#define LOG_FILE      MOUNT_POINT "/ACCESS.LOG"   /* 8.3 name: works without long file names */

/* Fixed ring buffer in RAM: keeps the most recent STORAGE_RING event log lines */
static char              s_log[STORAGE_RING][STORAGE_LINE_LEN];

/* Total count of log lines recorded since system boot (used as sequence ID) */
static int               s_count;

/* true while the SD card is mounted and writable */
static bool              s_sd_ok;

/* One mutex guards the RAM ring and the log file, so lines from
 * different tasks are never mixed */
static SemaphoreHandle_t s_lock;

/* Mounts the SD card on SPI2 as a FAT file system at /sdcard. */
static esp_err_t sd_mount(void)
{
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();     /* SPI2_HOST */
    spi_bus_config_t bus = {
        .mosi_io_num     = PIN_SD_MOSI,
        .miso_io_num     = PIN_SD_MISO,
        .sclk_io_num     = PIN_SD_SCK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 4000,
    };
    esp_err_t e = spi_bus_initialize(host.slot, &bus, SDSPI_DEFAULT_DMA);
    if (e != ESP_OK) return e;

    sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev.gpio_cs = PIN_SD_CS;
    dev.host_id = host.slot;

    esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = true,
        .max_files              = 2,
        .allocation_unit_size   = 16 * 1024,
    };
    sdmmc_card_t *card;
    e = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &dev, &mount, &card);
    if (e != ESP_OK) {
        spi_bus_free(host.slot);
    }
    return e;
}

/**
 * Initializes the storage subsystem:
 * 1. Creates the mutex lock.
 * 2. Initializes ESP32 Non-Volatile Storage (NVS) in flash memory.
 *    If NVS partition is corrupted or unformatted, erases and re-initializes.
 * 3. Mounts the SD card. If that fails the log stays in RAM only.
 */
esp_err_t storage_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;

    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        e = nvs_flash_erase();
        if (e == ESP_OK) {
            e = nvs_flash_init();
        }
    }
    printf("[STORAGE] settings in NVS: %s\n", e == ESP_OK ? "OK" : esp_err_to_name(e));
    if (e != ESP_OK) return e;

    esp_err_t sd = sd_mount();
    s_sd_ok = (sd == ESP_OK);
    if (s_sd_ok) {
        printf("[STORAGE] log mode: SD card (%s) + RAM ring of %d lines\n", LOG_FILE, STORAGE_RING);
    } else {
        printf("[STORAGE] log mode: RAM ONLY, last %d lines (SD card failed: %s)\n",
               STORAGE_RING, esp_err_to_name(sd));
    }
    return ESP_OK;
}

bool storage_sd_mounted(void)
{
    return s_sd_ok;
}

/**
 * Appends a log line as "<seq>,<line>" to the RAM ring and, if mounted,
 * to the SD log file. seq restarts at 1 after every boot.
 */
void storage_log(const char *line)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_count++;
    char *slot = s_log[s_count % STORAGE_RING];
    snprintf(slot, STORAGE_LINE_LEN, "%d,%s", s_count, line);

    if (s_sd_ok) {
        FILE *f = fopen(LOG_FILE, "a");
        if (f != NULL) {
            fprintf(f, "%s\n", slot);
            fclose(f);
        } else {
            s_sd_ok = false;
            printf("[STORAGE] cannot write %s, log is now RAM only\n", LOG_FILE);
        }
    }
    xSemaphoreGive(s_lock);
}

/**
 * Prints the last n lines kept in RAM, oldest first.
 * The lines are copied under the mutex and printed after releasing it,
 * so a slow serial port never blocks the tasks that are logging.
 */
void storage_log_print(int n)
{
    if (n <= 0 || n > STORAGE_RING) n = STORAGE_RING;
    char (*copy)[STORAGE_LINE_LEN] = malloc((size_t)n * STORAGE_LINE_LEN);
    if (copy == NULL) {
        printf("(out of memory)\n");
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int first = s_count - n + 1;
    if (first < 1) first = 1;
    int count = 0;
    for (int i = first; i <= s_count; i++) {
        memcpy(copy[count++], s_log[i % STORAGE_RING], STORAGE_LINE_LEN);
    }
    xSemaphoreGive(s_lock);

    printf("-- last %d log lines (RAM) --\n", count);
    for (int i = 0; i < count; i++) {
        printf("%s\n", copy[i]);
    }
    if (count == 0) {
        printf("(log is empty)\n");
    }
    free(copy);
}

/**
 * Prints the last max_lines lines of the SD log file.
 * Reads the file once, keeping only the newest lines in a small ring,
 * then prints them after releasing the mutex.
 */
void storage_log_dump_sd(int max_lines)
{
    if (!s_sd_ok) {
        printf("(SD card not mounted)\n");
        return;
    }
    if (max_lines <= 0 || max_lines > STORAGE_RING) max_lines = STORAGE_RING;
    char (*tail)[STORAGE_LINE_LEN] = malloc((size_t)max_lines * STORAGE_LINE_LEN);
    if (tail == NULL) {
        printf("(out of memory)\n");
        return;
    }

    int total = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    FILE *f = fopen(LOG_FILE, "r");
    if (f != NULL) {
        char buf[STORAGE_LINE_LEN + 2];           /* room for "\n" and '\0' */
        while (fgets(buf, sizeof(buf), f) != NULL) {
            buf[strcspn(buf, "\n")] = '\0';
            buf[STORAGE_LINE_LEN - 1] = '\0';
            memcpy(tail[total % max_lines], buf, STORAGE_LINE_LEN);
            total++;
        }
        fclose(f);
    }
    xSemaphoreGive(s_lock);

    int shown = total < max_lines ? total : max_lines;
    printf("-- last %d of %d lines in %s --\n", shown, total, LOG_FILE);
    for (int i = total - shown; i < total; i++) {
        printf("%s\n", tail[i % max_lines]);
    }
    free(tail);
}

/**
 * Writes an arbitrary data blob into flash NVS under the "cocis" namespace.
 * Used to persist settings like schedule hours and the admin PIN hash.
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
 * Returns ESP_ERR_NOT_FOUND if the key (or the namespace) does not exist yet.
 */
esp_err_t storage_kv_get(const char *key, void *data, size_t *len)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open("cocis", NVS_READONLY, &h);
    if (e == ESP_OK) {
        e = nvs_get_blob(h, key, data, len);
        nvs_close(h);
    }
    return (e == ESP_ERR_NVS_NOT_FOUND) ? ESP_ERR_NOT_FOUND : e;
}
