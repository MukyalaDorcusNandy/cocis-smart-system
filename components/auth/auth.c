#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "storage.h"
#include "auth.h"

/* Key name for storing the Admin PIN in NVS flash memory */
#define PIN_KEY       "adm_pin"

/* Default fallback PIN for initial boot */
#define DEFAULT_PIN   "1234"

/* Maximum allowed PIN length */
#define PIN_MAX_LEN   8

/* In-memory buffer storing current admin PIN */
static char              s_pin[PIN_MAX_LEN + 1];

/* Session state flag: true when an admin is currently logged in */
static bool              s_logged_in = false;

/* Mutex lock protecting PIN checks and session state */
static SemaphoreHandle_t s_lock;

/**
 * Initializes the authentication module:
 * Reads saved PIN from NVS. If not found (first boot), writes DEFAULT_PIN ("1234").
 */
esp_err_t auth_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    size_t len = sizeof(s_pin);

    /* Try to fetch saved PIN from NVS */
    if (storage_kv_get(PIN_KEY, s_pin, &len) != ESP_OK || len == 0) {
        strncpy(s_pin, DEFAULT_PIN, sizeof(s_pin));
        storage_kv_set(PIN_KEY, s_pin, strlen(s_pin) + 1);
    }
    return ESP_OK;
}

/**
 * Validates provided PIN against stored PIN.
 * If correct, sets logged-in session flag to true.
 */
bool auth_login(const char *pin)
{
    if (!pin) return false;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = (strcmp(s_pin, pin) == 0);
    if (ok) {
        s_logged_in = true;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

/**
 * Ends the active admin session.
 */
void auth_logout(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_logged_in = false;
    xSemaphoreGive(s_lock);
}

/**
 * Checks whether the current serial session is authenticated as Admin.
 */
bool auth_is_logged_in(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool logged = s_logged_in;
    xSemaphoreGive(s_lock);
    return logged;
}

/**
 * Updates the admin PIN:
 * Validates that the PIN is 4-8 digits, updates memory, and saves to NVS.
 */
esp_err_t auth_set_pin(const char *new_pin)
{
    if (!new_pin) return ESP_ERR_INVALID_ARG;
    size_t len = strlen(new_pin);
    if (len < 4 || len > PIN_MAX_LEN) return ESP_ERR_INVALID_ARG;

    /* Verify all characters are digits */
    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)new_pin[i])) return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    strncpy(s_pin, new_pin, sizeof(s_pin));
    s_pin[sizeof(s_pin) - 1] = '\0';
    esp_err_t err = storage_kv_set(PIN_KEY, s_pin, strlen(s_pin) + 1);
    xSemaphoreGive(s_lock);
    return err;
}
