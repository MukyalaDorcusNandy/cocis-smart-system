#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "esp_err.h"
#include "room_control.h"
#include "access_control.h"

static volatile bool show_room_status = false;   /* start quiet: type "roomlog on" to show */

/* Room status callback (Arnold's rooms report through this) */
static void on_room_status(const room_status_t *s)
{
    if (!show_room_status) return;
    printf("[STATUS] room=%u occupied=%d light=%d light_on_s=%lu seq=%lu\n",
           s->room, s->occupied, s->light_on,
           (unsigned long)s->light_on_s, (unsigned long)s->seq);
}

/* Console command: roomlog on|off */
static void cmd_roomlog(const char *args)
{
    show_room_status = (strncmp(args, "off", 3) != 0);
    printf("room status lines: %s\n", show_room_status ? "ON" : "OFF");
}

void app_main(void)
{
    room_control_set_publish_cb(on_room_status);
    ESP_ERROR_CHECK(room_control_start(1));
    ESP_ERROR_CHECK(room_control_start(2));
    printf("\n=== CoCIS: rooms 1 and 2 running ===\n");
}