#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "access_control.h"
#include "schedule.h"
#include "auth.h"
#include "storage.h"
#include "management.h"

#define SYSLOG_DEFAULT_LINES 10

/* Every protected command starts with this check. auth_is_logged_in() also
 * applies the idle timeout and restarts the idle timer. */
static bool require_login(void)
{
    if (auth_is_logged_in()) return true;
    printf("login required: login <pin>\n");
    return false;
}

/* "HH:MM" -> minutes after midnight */
static bool parse_hhmm(const char *s, int *minutes)
{
    int h, m;
    if (sscanf(s, "%d:%d", &h, &m) != 2) return false;
    if (h < 0 || h > 23 || m < 0 || m > 59) return false;
    *minutes = h * 60 + m;
    return true;
}

static void lower(char *s)
{
    for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}

static void print_hours(void)
{
    int o, c;
    schedule_get(&o, &c);
    printf("opening hours %02d:%02d - %02d:%02d, building is %s\n",
           o / 60, o % 60, c / 60, c % 60, schedule_is_open() ? "OPEN" : "CLOSED");
}

/* ---- commands ---- */

static void cmd_login(const char *args)
{
    char pin[16];
    if (sscanf(args, "%15s", pin) != 1) {
        printf("usage: login <pin>\n");
        return;
    }
    if (auth_login(pin)) {
        printf("logged in as admin (session ends after %d s without a [login] command)\n",
               AUTH_IDLE_TIMEOUT_S);
        if (auth_must_change_pin()) {
            printf("WARNING: default PIN in use, run: setpin <new 4-8 digits>\n");
        }
    } else {
        int left = auth_lockout_remaining_s();
        if (left > 0) {
            printf("login locked, try again in %d s\n", left);
        } else {
            printf("wrong PIN\n");
        }
    }
    memset(pin, 0, sizeof(pin));
}

static void cmd_logout(const char *args)
{
    auth_logout();
    printf("logged out\n");
}

static void cmd_setpin(const char *args)
{
    if (!require_login()) return;
    char pin[16];
    if (sscanf(args, "%15s", pin) != 1) {
        printf("usage: setpin <new 4-8 digits>\n");
        return;
    }
    esp_err_t e = auth_set_pin(pin);
    memset(pin, 0, sizeof(pin));
    if (e == ESP_OK) {
        printf("PIN changed\n");
    } else if (e == ESP_ERR_INVALID_ARG) {
        printf("PIN must be 4-8 digits\n");
    } else {
        printf("PIN not changed: %s\n", esp_err_to_name(e));
    }
}

static void cmd_hours(const char *args)
{
    char a[16], b[16];
    int n = sscanf(args, "%15s %15s", a, b);
    if (n <= 0) {                                 /* no arguments: show */
        print_hours();
        return;
    }
    if (!require_login()) return;
    int o, c;
    if (n != 2 || !parse_hhmm(a, &o) || !parse_hhmm(b, &c)) {
        printf("usage: hours HH:MM HH:MM   (open, close)\n");
        return;
    }
    esp_err_t e = schedule_set(o, c);
    if (e == ESP_ERR_INVALID_ARG) {
        printf("opening time must be before closing time\n");
        return;
    }
    if (e != ESP_OK) {
        printf("hours set but not saved: %s\n", esp_err_to_name(e));
    }
    schedule_log("SCHEDULE,hours set to %02d:%02d-%02d:%02d", o / 60, o % 60, c / 60, c % 60);
    print_hours();
}

static void cmd_clock(const char *args)
{
    char a[16], now[16];
    if (sscanf(args, "%15s", a) != 1) {           /* no argument: show */
        printf("clock %s\n", clock_str(now, sizeof(now)));
        return;
    }
    if (!require_login()) return;
    int m;
    if (!parse_hhmm(a, &m)) {
        printf("usage: clock HH:MM\n");
        return;
    }
    clock_str(now, sizeof(now));
    clock_set(m / 60, m % 60);
    schedule_log("CLOCK,set from %s to %02d:%02d", now, m / 60, m % 60);
    printf("clock %s\n", clock_str(now, sizeof(now)));
    print_hours();
}

static void cmd_syslog(const char *args)
{
    if (!require_login()) return;
    int n = SYSLOG_DEFAULT_LINES;
    if (sscanf(args, "%d", &n) != 1 || n <= 0) n = SYSLOG_DEFAULT_LINES;
    storage_log_print(n);
    if (storage_sd_mounted()) {
        storage_log_dump_sd(n);
    } else {
        printf("(SD card not mounted: the log is in RAM only)\n");
    }
}

static void cmd_emergency(const char *args)
{
    if (!require_login()) return;
    char a[8] = "";
    sscanf(args, "%7s", a);
    lower(a);
    bool on;
    if (strcmp(a, "on") == 0) {
        on = true;
    } else if (strcmp(a, "off") == 0) {
        on = false;
    } else {
        printf("usage: emergency on|off\n");
        return;
    }
    entrance_set_emergency(on);
    schedule_log("EMERGENCY,%s", on ? "on" : "off");
    printf("emergency mode %s%s\n", on ? "ON" : "off",
           on ? ": exit allowed outside opening hours" : "");
}

static void block_or_unblock(const char *args, bool block)
{
    if (!require_login()) return;
    char uid[ENTRANCE_UID_LEN];
    if (sscanf(args, "%15s", uid) != 1) {
        printf("usage: %s <UID>\n", block ? "block" : "unblock");
        return;
    }
    esp_err_t e = entrance_set_blocked(uid, block);
    if (e == ESP_ERR_NOT_FOUND) {
        printf("unknown card %s\n", uid);
        return;
    }
    if (e != ESP_OK) {
        printf("changed but not saved: %s\n", esp_err_to_name(e));
    }
    schedule_log("CARD,%s,%s", uid, block ? "blocked" : "unblocked");
    printf("card %s %s\n", uid, block ? "BLOCKED" : "unblocked");
}

static void cmd_block(const char *args)   { block_or_unblock(args, true); }
static void cmd_unblock(const char *args) { block_or_unblock(args, false); }

esp_err_t management_register_commands(void)
{
    static const struct {
        const char        *name;
        const char        *help;
        entrance_cmd_fn_t  fn;
    } cmds[] = {
        { "login",     "login <pin>         start an admin session (3 wrong PINs = 30 s lockout)", cmd_login },
        { "logout",    "logout              end the admin session",                                cmd_logout },
        { "setpin",    "setpin <new>        [login] change the admin PIN (4-8 digits)",            cmd_setpin },
        { "hours",     "hours [HH:MM HH:MM] show opening hours; with times: [login] set them",     cmd_hours },
        { "clock",     "clock [HH:MM]       show the clock; with a time: [login] set it",          cmd_clock },
        { "syslog",    "syslog [n]          [login] last n system log lines (RAM and SD card)",    cmd_syslog },
        { "emergency", "emergency on|off    [login] allow exit outside opening hours",             cmd_emergency },
        { "block",     "block <UID>         [login] block a lost or stolen card",                  cmd_block },
        { "unblock",   "unblock <UID>       [login] unblock a card",                               cmd_unblock },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        esp_err_t e = entrance_register_command(cmds[i].name, cmds[i].help, cmds[i].fn);
        if (e != ESP_OK) return e;
    }
    return ESP_OK;
}
