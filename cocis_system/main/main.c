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

    /* 4. the three terminals; the entrance is last because it starts the
     *    console (the only reader of the serial port) and prints the help */
    ESP_ERROR_CHECK(room_control_start(1));
    ESP_ERROR_CHECK(room_control_start(2));
    ESP_ERROR_CHECK(entrance_start());
}
