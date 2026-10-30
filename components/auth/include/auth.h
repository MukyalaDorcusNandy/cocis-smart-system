#pragma once
/* auth - simple admin PIN authentication for privileged management commands.
 * Admin PIN is persisted in NVS (default "1234").
 * A simple PIN check is used for this classroom prototype. */
#include <stdbool.h>
#include "esp_err.h"

esp_err_t auth_init(void);               /* loads PIN from NVS or sets default */
bool      auth_login(const char *pin);   /* returns true if PIN matches */
void      auth_logout(void);             /* ends the admin session */
bool      auth_is_logged_in(void);       /* checks if currently authenticated */
esp_err_t auth_set_pin(const char *new_pin); /* changes admin PIN (4-8 digits) */
