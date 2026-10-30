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
 * RFID is simulated: type "in <UID>" or "out <UID>" in the serial monitor.
 */
#include <stdbool.h>
#include "esp_err.h"

#define ENTRANCE_UID_LEN   16
#define ENTRANCE_NAME_LEN  24

/* Starts the entrance node (door servos, LEDs, buzzer, console). Call once. */
esp_err_t entrance_start(void);

/* ---- Hook 1: be told about every scan decision (for the hub / logging) ---- */
typedef struct {
    bool        is_entry;                      /* true = ENTRY reader, false = EXIT reader */
    char        uid[ENTRANCE_UID_LEN];
    char        name[ENTRANCE_NAME_LEN];       /* "-" when the tag is unknown              */
    bool        granted;
    const char *reason;                        /* fixed string, e.g. "ok", "tag blocked"  */
    int         inside_count;                  /* people inside AFTER this decision        */
} entrance_event_t;

/* Called from the entrance task after every decision. Keep it short and never
 * block (for example, push the event into a queue). Set it before entrance_start(). */
typedef void (*entrance_event_cb_t)(const entrance_event_t *ev);
void entrance_set_event_cb(entrance_event_cb_t cb);

/* ---- Hook 2: add serial commands (login, rooms, ...) ---- */
/* 'args' is the rest of the typed line after the command word ("" if none).
 * The command word must be lower case. Up to 8 commands. Call before entrance_start(). */
typedef void (*entrance_cmd_fn_t)(const char *args);
esp_err_t entrance_register_command(const char *name, const char *help,
                                    entrance_cmd_fn_t fn);

/* Number of people inside the building right now (thread safe). */
int entrance_inside_count(void);
