#include <stdio.h>
#include "room_control.h"

static void on_room_status(const room_status_t *s)
{
    printf("[STATUS] room=%u occupied=%d light=%d light_on_s=%lu seq=%lu\n",
           s->room, s->occupied, s->light_on,
           (unsigned long)s->light_on_s, (unsigned long)s->seq);
}

void app_main(void)
{
    room_control_set_publish_cb(on_room_status);
    ESP_ERROR_CHECK(room_control_start(2));
    printf("\n=== CoCIS: room 2 running ===\n");
}