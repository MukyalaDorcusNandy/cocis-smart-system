/* cocis_system - the one firmware for the whole system (owner: Kiran).
 * One ESP32 runs the entrance terminal and lecture rooms 1 and 2 as FreeRTOS
 * tasks. They report to the hub task (hub.c) through a queue.
 * This file only starts things, in the order below. */
#include "esp_err.h"
#include "storage.h"
#include "schedule.h"
#include "auth.h"
#include "management.h"
#include "room_control.h"
#include "access_control.h"
#include "hub.h"
#include "rfid.h"
#include "web_dashboard.h"
#include "lcd_display.h"

/* Adapter for the web dashboard: it cannot call the hub (main/ is not a
 * component), so it asks through this function. */
static bool web_room(int room, bool *online, bool *occupied, bool *light, uint32_t *light_on_s)
{
    hub_room_t r;
    if (!hub_get_room((uint8_t)room, &r)) return false;   /* no such room */
    *online     = r.online;
    *occupied   = r.status.occupied;
    *light      = r.status.light_on;
    *light_on_s = r.status.light_on_s;
    return true;
}

void app_main(void)
{
    /* 1. shared services, in this order: storage -> schedule -> auth */
    ESP_ERROR_CHECK(storage_init());
    ESP_ERROR_CHECK(schedule_init());
    ESP_ERROR_CHECK(auth_init());
    schedule_log("SYSTEM,boot");

    /* 2. hub: queue, task, room/entrance callbacks, "rooms" command */
    ESP_ERROR_CHECK(hub_start());

    /* 3. admin commands on the console (login, hours, clock, syslog, ...) */
    ESP_ERROR_CHECK(management_register_commands());

    /* web dashboard: room adapter, alert listener and "wifi" command
     * (must be before entrance_start, which starts the console) */
    web_dashboard_set_room_provider(web_room);
    ESP_ERROR_CHECK(web_dashboard_register_commands());

    /* "rfid" reader-status command (before entrance_start, like every command) */
    ESP_ERROR_CHECK(rfid_register_commands());

    /* entrance LCD: adds an entrance listener, so it must be before entrance_start */
    ESP_ERROR_CHECK(lcd_display_start());

    /* 4. the three terminals; the entrance is last because it starts the
     *    console (the only reader of the serial port) and prints the help */
    ESP_ERROR_CHECK(room_control_start(1));
    ESP_ERROR_CHECK(room_control_start(2));
    ESP_ERROR_CHECK(entrance_start());

    /* 5. the two MFRC522 RFID readers (ENTRY, EXIT); they send scans to the
     *    entrance, so they start after it */
    ESP_ERROR_CHECK(rfid_start());

    /* 6. Wi-Fi + web dashboard (does not wait for Wi-Fi; port 80 -> localhost:8180) */
    ESP_ERROR_CHECK(web_dashboard_start());
}
