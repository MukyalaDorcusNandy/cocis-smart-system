#pragma once
/*
 * room_control - lecture-room logic, shared by Room 1 and Room 2.
 * One call per room starts one task:
 *     room_control_set_publish_cb(my_cb);   // once, before starting rooms
 *     room_control_start(1);
 *     room_control_start(2);
 * It knows nothing about the entrance code: it just calls the publish hook
 * on every change and every ROOM_HEARTBEAT_MS.
 */
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* One room status report. */
typedef struct {
    uint8_t  room;          /* 1 or 2                                       */
    bool     occupied;      /* motion seen within the vacancy delay         */
    bool     light_on;      /* light output state                           */
    uint32_t light_on_s;    /* cumulative seconds the light has been on     */
    uint32_t seq;           /* +1 for every report of that room             */
} room_status_t;

/* Called from the room task(s). Keep it short and never block for long. */
typedef void (*room_publish_cb_t)(const room_status_t *status);

void      room_control_set_publish_cb(room_publish_cb_t cb);
esp_err_t room_control_start(uint8_t room);                    /* 1..ROOM_COUNT */
bool      room_control_get(uint8_t room, room_status_t *out);  /* latest status */
