#pragma once
/* auth - admin PIN login for the management commands.
 *  - One role only: ADMIN. The coursework asks that only authenticated,
 *    authorised staff change settings; whoever knows the admin PIN is that person.
 *  - The PIN is never stored: NVS holds a random 16-byte salt and SHA-256(salt + PIN).
 *  - 3 wrong PINs in a row lock login for 30 s.
 *  - A session ends after 120 s without a protected command (idle timeout).
 *  - First boot only: the PIN is set to 1234 and must be changed with setpin.
 *  - Every login, failure, lockout, logout, timeout and PIN change is logged.
 *
 * Init order in app_main: storage_init(), schedule_init(), auth_init(). */
#include <stdbool.h>
#include "esp_err.h"

#define AUTH_MAX_TRIES       3
#define AUTH_LOCKOUT_S       30
#define AUTH_IDLE_TIMEOUT_S  120

esp_err_t auth_init(void);                   /* loads the PIN hash or creates the default */
bool      auth_login(const char *pin);       /* true if the PIN is right and login is not locked */
void      auth_logout(void);                 /* ends the admin session */

/* true while a session is open. Applies the idle timeout, and restarts the
 * idle timer, so call it at the start of every protected command. */
bool      auth_is_logged_in(void);

/* Changes the PIN (4-8 digits). Needs an open session. */
esp_err_t auth_set_pin(const char *new_pin);

int       auth_lockout_remaining_s(void);    /* 0 when login is allowed */
bool      auth_must_change_pin(void);        /* true while the default PIN is in use */
