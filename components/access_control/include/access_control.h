#pragma once
/*
 * access_control - entrance and RFID access control (owner: Dorcus).
 *
 * Minimum use, from cocis_system/main/main.c:
 *     ESP_ERROR_CHECK(entrance_start());
 *
 * entrance_start() creates the entrance tasks and returns. It also owns the
 * serial console (UART0), so no other component should read from the serial
 * port. Other components add their own commands with entrance_register_command().
 *
 * Card scans come from the two MFRC522 readers (rfid component), from typing
 * "in <UID>" / "out <UID>" in the serial monitor, or from the dashboard's test
 * scan. All three go through entrance_submit_scan() onto scan_queue.
 * Opening hours and the clock come from the schedule component.
 */
#include <stdbool.h>
#include "esp_err.h"

#define ENTRANCE_UID_LEN   16
#define ENTRANCE_NAME_LEN  24

/* Every card has a role, and every role has a card colour. The colour is shown
 * by the Wokwi card itself, the entrance LCD, the console and the web dashboard, so
 * security can tell at a glance who is inside (blue = student, ...). */
typedef enum {
    ROLE_STUDENT = 0,   /* blue   */
    ROLE_STAFF,         /* green  */
    ROLE_GUEST,         /* yellow */
    ROLE_ADMIN,         /* red    */
    ROLE_COUNT
} entrance_role_t;

const char *entrance_role_name(entrance_role_t role);   /* "Student", ...  */
const char *entrance_role_color(entrance_role_t role);  /* "blue", ...     */

/* Starts the entrance node (door servos, LEDs, buzzer, console). Call once. */
esp_err_t entrance_start(void);

/* ---- Hook 1: be told about every scan decision (for the hub / logging) ---- */
typedef struct {
    bool        is_entry;                      /* true = ENTRY reader, false = EXIT reader */
    char        uid[ENTRANCE_UID_LEN];
    char        name[ENTRANCE_NAME_LEN];       /* "-" when the tag is unknown              */
    entrance_role_t role;                      /* only meaningful for a known tag          */
    bool        known;                         /* false when the tag is not registered     */
    bool        granted;
    const char *reason;                        /* fixed string, e.g. "ok", "tag blocked"  */
    int         inside_count;                  /* people inside AFTER this decision        */
} entrance_event_t;

/* Called from the entrance task after every decision. Keep it short and never
 * block (for example, push the event into a queue). Up to 4 listeners (the hub,
 * the web dashboard, ...); each call adds one. Call before entrance_start(). */
typedef void (*entrance_event_cb_t)(const entrance_event_t *ev);
void entrance_set_event_cb(entrance_event_cb_t cb);

/* ---- Hook 2: add serial commands (login, rooms, ...) ---- */
/* 'args' is the rest of the typed line after the command word ("" if none).
 * The command word must be lower case and unique: a name already used by a
 * built-in or registered command returns ESP_ERR_INVALID_STATE.
 * 'help' is the whole line shown by "help", e.g. "logout              end the admin session".
 * Up to 16 commands. Call before entrance_start(). */
typedef void (*entrance_cmd_fn_t)(const char *args);
esp_err_t entrance_register_command(const char *name, const char *help,
                                    entrance_cmd_fn_t fn);

/* Number of people inside the building right now (thread safe). */
int entrance_inside_count(void);

/* Emergency mode: exit is allowed outside opening hours (thread safe).
 * Shown by the "status" command. The login check is done by the caller
 * (management component). */
void entrance_set_emergency(bool on);
bool entrance_get_emergency(void);      /* current emergency mode (thread safe) */

/* Blocks or unblocks a registered card; saved in NVS. ESP_ERR_NOT_FOUND if the
 * UID is not registered. The login check is done by the caller. */
esp_err_t entrance_set_blocked(const char *uid, bool blocked);

/* ---- Hook 3: card readers ---- */
/* Queue one card scan, exactly like "in <UID>" / "out <UID>" on the console.
 * Used by the MFRC522 reader task (rfid component) and the web dashboard.
 * Never blocks: ESP_ERR_TIMEOUT if the scan queue is full,
 * ESP_ERR_INVALID_STATE before entrance_start(). */
esp_err_t entrance_submit_scan(bool is_entry, const char *uid);

/* ---- Hook 4: read-only snapshot of the card table (dashboard, reports) ---- */
typedef struct {
    char            uid[ENTRANCE_UID_LEN];
    char            name[ENTRANCE_NAME_LEN];
    entrance_role_t role;
    bool            blocked;
    bool            inside;
    char            since[16];      /* "HH:MM:SS" of the last granted scan, "" if none */
} entrance_user_t;

/* Copies up to 'max' users into 'out' (thread safe). Returns how many. */
int entrance_get_users(entrance_user_t *out, int max);
