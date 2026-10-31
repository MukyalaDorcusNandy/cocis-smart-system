#pragma once
/* Lecture-room settings. ONE ESP32 runs the entrance AND both rooms, so each
 * room needs its own pins. Pins already used by the entrance code: 18, 19
 * (servos), 25, 26, 27 (LEDs/buzzer). The SD card (storage) uses
 * 13, 14, 34 and 4. Change a value here only after telling the group. */
#define ROOM_COUNT              2

#define ROOM_PIR_GPIO(room)     ((room) == 1 ? 32 : 33)   /* PIR sensor OUT  */
#define ROOM_LED_GPIO(room)     ((room) == 1 ? 21 : 22)   /* light output    */

#define ROOM_POLL_MS            100
#define ROOM_VACANCY_DELAY_MS   10000   /* lights off this long after last motion
                                           (PIR sees motion, not presence).
                                           Demo value; ~300000 for real use.  */
#define ROOM_HEARTBEAT_MS       5000    /* status is re-sent this often       */
