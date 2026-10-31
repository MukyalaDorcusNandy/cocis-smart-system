#pragma once
/* management - serial commands for the admin (owner: Joshua).
 * Adds these commands to the entrance console ([login] = admin session needed):
 *   login <pin>, logout, setpin <new> [login],
 *   hours [HH:MM HH:MM] (setting needs [login]), clock [HH:MM] (setting needs [login]),
 *   syslog [n] [login], emergency on|off [login], block <UID> [login], unblock <UID> [login].
 * Every change is written to the system log (schedule_log).
 *
 * Call after storage_init/schedule_init/auth_init and before entrance_start(). */
#include "esp_err.h"

esp_err_t management_register_commands(void);
