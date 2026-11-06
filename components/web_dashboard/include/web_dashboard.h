#pragma once
/* web_dashboard - Wi-Fi + web security dashboard with PIN sign-in.
 *
 * Use from app_main:
 *     web_dashboard_set_room_provider(my_room_fn);   // optional, rooms panel
 *     web_dashboard_register_commands();             // BEFORE entrance_start()
 *     entrance_start();
 *     web_dashboard_start();                         // AFTER entrance_start()
 *
 * web_dashboard_start() starts Wi-Fi (station, "Wokwi-GUEST") and an HTTP
 * server on port 80. It does not wait for Wi-Fi: the rest of the system works
 * without it, and Wi-Fi keeps retrying in the background.
 * In Wokwi, port 80 is forwarded to http://localhost:8180 (see wokwi.toml).
 *
 * Pages / API (all JSON except "/"):
 *   GET  /               the dashboard page (dashboard.html, embedded)
 *   POST /api/login      pin=1234            -> {"ok":true,"token":"..."}
 *   POST /api/logout
 *   GET  /api/state      building, users, rooms, alerts      [token]
 *   POST /api/hours      open=HH:MM&close=HH:MM               [token]
 *   POST /api/emergency  on=1|0                               [token]
 *   POST /api/block      uid=XXXX&blocked=1|0                 [token]
 *   POST /api/scan       uid=XXXX&entry=1|0 (test card scan)  [token]
 * [token] = header "X-Token: <token from login>"; otherwise 401. */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* The hub lives in main/, so this component cannot call it directly. main.c
 * gives us a small function instead. Return false when 'room' does not exist
 * (rooms are numbered 1, 2, ...). */
typedef bool (*web_room_fn_t)(int room, bool *online, bool *occupied,
                              bool *light, uint32_t *light_on_s);
void web_dashboard_set_room_provider(web_room_fn_t fn);

/* Adds the "wifi" console command and the alert listener.
 * Call before entrance_start(). */
esp_err_t web_dashboard_register_commands(void);

/* Starts Wi-Fi and the web server. Call after entrance_start(). */
esp_err_t web_dashboard_start(void);

/* ---- Wi-Fi part (wifi.c) ---- */
esp_err_t wifi_net_start(void);                 /* non-blocking connect + retry */
bool      wifi_net_connected(void);
const char *wifi_net_ip(char *buf, int len);    /* "x.x.x.x" or "-" */
