#pragma once
/* hub - where the three terminals meet (owner: Kiran).
 * Room 1, room 2 and the entrance send their reports into ONE FreeRTOS queue.
 * hub_task is the only reader: it keeps the latest status of each room,
 * marks a room OFFLINE after HUB_OFFLINE_MS without a report, and logs
 * every change (schedule_log -> SD card). */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "room_control.h"

#define HUB_OFFLINE_MS  15000   /* 3 missed heartbeats (rooms report every 5 s) */

typedef struct {
    bool          online;        /* a report arrived in the last HUB_OFFLINE_MS */
    bool          ever_seen;     /* at least one report since boot */
    room_status_t status;        /* latest report */
    uint32_t      last_seen_ms;  /* uptime of the latest report */
} hub_room_t;

/* Creates the queue, the mutex and hub_task, registers the room and entrance
 * callbacks and the "rooms" / "roomfail" commands.
 * Call before room_control_start() and entrance_start(). */
esp_err_t hub_start(void);

/* Copy of the hub's view of one room (1..ROOM_COUNT). false if room is invalid. */
bool hub_get_room(uint8_t room, hub_room_t *out);
