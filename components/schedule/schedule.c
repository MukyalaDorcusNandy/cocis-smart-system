#include <stdio.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "storage.h"
#include "schedule.h"

/* NVS key name used to store open and close times */
#define KEY "sched"

/* Default access hours: 08:00 (480 mins) to 18:00 (1080 mins) */
#define DEFAULT_OPEN   (8 * 60)   /* 08:00 in minutes */
#define DEFAULT_CLOSE  (18 * 60)  /* 18:00 in minutes */
#define START_SEC      (8 * 3600) /* Clock starts at 08:00:00 at boot */

/* Structure to hold schedule times in minutes from midnight (0 - 1440) */
typedef struct {
    int16_t open_min;
    int16_t close_min;
} sched_t;

/* In-memory copy of current schedule */
static sched_t           s_sched = { DEFAULT_OPEN, DEFAULT_CLOSE };

/* Mutex lock to prevent race conditions between CLI and access tasks */
static SemaphoreHandle_t s_lock;

/* Simulated clock tracking: base simulated time in seconds */
static int64_t           s_base_sim_s = START_SEC;

/* Real ESP32 uptime (in seconds) when s_base_sim_s was set */
static int64_t           s_base_real_s = 0;

/**
 * Calculates current simulated time in seconds past midnight (0 .. 86399).
 * Formula: (base_simulated_time + elapsed_real_time) % 24_hours
 */
static int64_t now_sim_s(void)
{
    int64_t real_s = esp_timer_get_time() / 1000000;
    return (s_base_sim_s + (real_s - s_base_real_s)) % 86400;
}

/**
 * Initializes the schedule module:
 * 1. Creates the mutex lock.
 * 2. Records initial boot uptime.
 * 3. Loads any previously saved schedule from NVS flash storage.
 */
esp_err_t schedule_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_base_real_s = esp_timer_get_time() / 1000000;

    sched_t tmp;
    size_t len = sizeof(tmp);
    /* Attempt to read saved schedule from NVS */
    if (storage_kv_get(KEY, &tmp, &len) == ESP_OK && len == sizeof(tmp)) {
        if (tmp.open_min >= 0 && tmp.close_min <= 1440 && tmp.open_min < tmp.close_min) {
            s_sched = tmp;
        }
    }
    printf("[SCHEDULE] open %02d:%02d, close %02d:%02d\n",
           s_sched.open_min / 60, s_sched.open_min % 60,
           s_sched.close_min / 60, s_sched.close_min % 60);
    return ESP_OK;
}

/**
 * Updates opening and closing hours, validates them, and saves to NVS.
 * Open time must be earlier than close time.
 */
esp_err_t schedule_set(int open_min, int close_min)
{
    if (open_min < 0 || close_min > 1440 || open_min >= close_min) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_sched.open_min = open_min;
    s_sched.close_min = close_min;
    sched_t copy = s_sched;
    xSemaphoreGive(s_lock);

    /* Save to NVS so the schedule is remembered after reboot */
    return storage_kv_set(KEY, &copy, sizeof(copy));
}

/**
 * Retrieves the current configured open and close times in minutes.
 */
void schedule_get(int *open_min, int *close_min)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *open_min = s_sched.open_min;
    *close_min = s_sched.close_min;
    xSemaphoreGive(s_lock);
}

/**
 * Returns current simulated time converted into minutes from midnight (0..1439).
 */
int clock_minutes(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int m = (int)(now_sim_s() / 60);
    xSemaphoreGive(s_lock);
    return m;
}

/**
 * Checks whether the building is currently within permitted operating hours.
 * Returns true if open, false if closed.
 */
bool schedule_is_open(void)
{
    int m = clock_minutes();
    int o, c;
    schedule_get(&o, &c);
    return (m >= o && m < c);
}

/**
 * Sets the simulated clock to a specific hour and minute.
 * Allows testing morning, evening, or night scenarios immediately in Wokwi.
 */
void clock_set(int hour, int minute)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_base_sim_s = hour * 3600 + minute * 60;
    s_base_real_s = esp_timer_get_time() / 1000000;
    xSemaphoreGive(s_lock);
}

/**
 * Formats the current simulated time into "HH:MM:SS" format.
 */
const char *clock_str(char *buf, int len)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int64_t s = now_sim_s();
    xSemaphoreGive(s_lock);
    snprintf(buf, len, "%02d:%02d:%02d", (int)(s / 3600), (int)((s % 3600) / 60), (int)(s % 60));
    return buf;
}

/**
 * Logs an event line prefixed with the current timestamp: "HH:MM:SS,<event>".
 * Writes directly into the storage circular buffer.
 */
void schedule_log(const char *fmt, ...)
{
    char text[64], line[80], t[16];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

    snprintf(line, sizeof(line), "%s,%s", clock_str(t, sizeof(t)), text);
    storage_log(line);
}
