#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "mbedtls/sha256.h"
#include "storage.h"
#include "schedule.h"
#include "auth.h"

/* NVS key for the stored PIN record (salt + hash, never the PIN itself) */
#define PIN_KEY       "pin_hash"

/* PIN created on first boot only; the admin is told to change it */
#define DEFAULT_PIN   "1234"

#define PIN_MIN_LEN   4
#define PIN_MAX_LEN   8
#define SALT_LEN      16
#define HASH_LEN      32

/* What is saved in NVS */
typedef struct {
    uint8_t salt[SALT_LEN];
    uint8_t hash[HASH_LEN];     /* SHA-256(salt + PIN) */
    uint8_t must_change;        /* 1 while the default PIN is in use */
} pin_record_t;

static pin_record_t      s_rec;

/* Session and lockout state */
static bool              s_logged_in = false;
static int64_t           s_last_activity_us;   /* last protected command */
static int               s_failures;           /* wrong PINs in a row */
static int64_t           s_locked_until_us;    /* login refused until this time */

/* Mutex protecting everything above */
static SemaphoreHandle_t s_lock;

static int64_t now_us(void)
{
    return esp_timer_get_time();
}

/* hash = SHA-256(salt + pin) */
static void hash_pin(const uint8_t salt[SALT_LEN], const char *pin, uint8_t out[HASH_LEN])
{
    uint8_t buf[SALT_LEN + PIN_MAX_LEN];
    size_t n = strnlen(pin, PIN_MAX_LEN);
    memcpy(buf, salt, SALT_LEN);
    memcpy(buf + SALT_LEN, pin, n);
    mbedtls_sha256(buf, SALT_LEN + n, out, 0);
    memset(buf, 0, sizeof(buf));
}

/* Compares two hashes in constant time (does not stop at the first difference) */
static bool hash_equal(const uint8_t *a, const uint8_t *b)
{
    uint8_t diff = 0;
    for (int i = 0; i < HASH_LEN; i++) {
        diff |= a[i] ^ b[i];
    }
    return diff == 0;
}

static bool pin_format_ok(const char *pin)
{
    size_t len = strnlen(pin, PIN_MAX_LEN + 1);
    if (len < PIN_MIN_LEN || len > PIN_MAX_LEN) return false;
    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)pin[i])) return false;
    }
    return true;
}

/* New random salt + hash of 'pin' into rec. */
static void make_record(pin_record_t *rec, const char *pin, bool must_change)
{
    esp_fill_random(rec->salt, SALT_LEN);
    hash_pin(rec->salt, pin, rec->hash);
    rec->must_change = must_change ? 1 : 0;
}

/**
 * Initializes the authentication module:
 * Loads the salted PIN hash from NVS. Only if the key does not exist yet
 * (first boot) it creates the default PIN and marks it "must change".
 * Any other NVS error is returned.
 */
esp_err_t auth_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;

    size_t len = sizeof(s_rec);
    esp_err_t e = storage_kv_get(PIN_KEY, &s_rec, &len);
    if (e == ESP_OK && len == sizeof(s_rec)) {
        return ESP_OK;
    }
    if (e != ESP_OK && e != ESP_ERR_NOT_FOUND) {
        printf("[AUTH] cannot read PIN record: %s\n", esp_err_to_name(e));
        return e;
    }

    /* first boot (or a record of the wrong size): create the default PIN */
    make_record(&s_rec, DEFAULT_PIN, true);
    e = storage_kv_set(PIN_KEY, &s_rec, sizeof(s_rec));
    if (e != ESP_OK) {
        printf("[AUTH] cannot save PIN record: %s\n", esp_err_to_name(e));
        return e;
    }
    printf("[AUTH] first boot: default admin PIN created, change it with setpin\n");
    schedule_log("AUTH,default PIN created");
    return ESP_OK;
}

int auth_lockout_remaining_s(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int64_t left = s_locked_until_us - now_us();
    xSemaphoreGive(s_lock);
    return left > 0 ? (int)((left + 999999) / 1000000) : 0;
}

/**
 * Checks the PIN against the stored hash.
 * Refused while locked. After AUTH_MAX_TRIES wrong PINs login is locked
 * for AUTH_LOCKOUT_S seconds.
 */
bool auth_login(const char *pin)
{
    if (pin == NULL) return false;
    if (auth_lockout_remaining_s() > 0) {
        schedule_log("AUTH,login refused (locked)");
        return false;
    }

    uint8_t h[HASH_LEN];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    hash_pin(s_rec.salt, pin, h);
    bool ok = pin_format_ok(pin) && hash_equal(h, s_rec.hash);
    bool locked_now = false;
    if (ok) {
        s_logged_in = true;
        s_failures = 0;
        s_last_activity_us = now_us();
    } else if (++s_failures >= AUTH_MAX_TRIES) {
        s_failures = 0;
        s_locked_until_us = now_us() + (int64_t)AUTH_LOCKOUT_S * 1000000;
        locked_now = true;
    }
    xSemaphoreGive(s_lock);

    if (ok) {
        schedule_log("AUTH,login OK");
    } else if (locked_now) {
        schedule_log("AUTH,login failed,locked for %d s", AUTH_LOCKOUT_S);
    } else {
        schedule_log("AUTH,login failed");
    }
    return ok;
}

/**
 * Ends the active admin session.
 */
void auth_logout(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool was = s_logged_in;
    s_logged_in = false;
    xSemaphoreGive(s_lock);
    if (was) schedule_log("AUTH,logout");
}

/**
 * Checks whether the serial session is logged in as admin.
 * Ends the session if it was idle too long; otherwise restarts the idle timer.
 */
bool auth_is_logged_in(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool timed_out = false;
    if (s_logged_in &&
        now_us() - s_last_activity_us > (int64_t)AUTH_IDLE_TIMEOUT_S * 1000000) {
        s_logged_in = false;
        timed_out = true;
    }
    if (s_logged_in) {
        s_last_activity_us = now_us();
    }
    bool logged = s_logged_in;
    xSemaphoreGive(s_lock);

    if (timed_out) {
        printf("session ended after %d s without activity\n", AUTH_IDLE_TIMEOUT_S);
        schedule_log("AUTH,session timeout");
    }
    return logged;
}

bool auth_must_change_pin(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool m = s_rec.must_change;
    xSemaphoreGive(s_lock);
    return m;
}

/**
 * Changes the admin PIN: needs an open session and 4-8 digits.
 * Saves a new salt + hash to NVS.
 */
esp_err_t auth_set_pin(const char *new_pin)
{
    if (new_pin == NULL || !pin_format_ok(new_pin)) return ESP_ERR_INVALID_ARG;
    if (!auth_is_logged_in()) return ESP_ERR_INVALID_STATE;

    pin_record_t rec;
    make_record(&rec, new_pin, false);
    esp_err_t err = storage_kv_set(PIN_KEY, &rec, sizeof(rec));
    if (err == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_rec = rec;
        xSemaphoreGive(s_lock);
        schedule_log("AUTH,PIN changed");
    }
    return err;
}
