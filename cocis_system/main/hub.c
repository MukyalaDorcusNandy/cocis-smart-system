#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "room_control.h"
#include "room_config.h"
#include "access_control.h"
#include "schedule.h"
#include "auth.h"
#include "hub.h"

#define HUB_QUEUE_LEN    16
#define HUB_STACK        6144    /* hub_task writes to the SD card (FAT) */

/* One message in the hub queue: a room report or an entrance decision */
typedef enum { HUB_MSG_ROOM, HUB_MSG_ENTRANCE } hub_msg_type_t;

typedef struct {
    hub_msg_type_t type;
    union {
        room_status_t    room;
        entrance_event_t entrance;
    };
} hub_msg_t;

static QueueHandle_t     s_queue;
static SemaphoreHandle_t s_lock;                 /* guards s_rooms and s_inside */
static hub_room_t        s_rooms[ROOM_COUNT];
static int               s_inside;               /* from the latest entrance report */

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ---- callbacks: run in the room / entrance tasks, so they only queue ---- */

static void on_room_status(const room_status_t *s)
{
    hub_msg_t m = { .type = HUB_MSG_ROOM, .room = *s };
    xQueueSend(s_queue, &m, 0);     /* never block a room; the 5 s heartbeat re-sends */
}

static void on_entrance_event(const entrance_event_t *ev)
{
    hub_msg_t m = { .type = HUB_MSG_ENTRANCE, .entrance = *ev };
    xQueueSend(s_queue, &m, 0);     /* the entrance logs its own decision */
}

/* ---- hub task ---- */

static void handle_room(const room_status_t *s)
{
    if (s->room < 1 || s->room > ROOM_COUNT) return;
    hub_room_t *r = &s_rooms[s->room - 1];

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool came_online = !r->online;
    bool changed = !r->ever_seen ||
                   r->status.occupied != s->occupied ||
                   r->status.light_on != s->light_on;
    r->status       = *s;
    r->last_seen_ms = now_ms();
    r->online       = true;
    r->ever_seen    = true;
    xSemaphoreGive(s_lock);

    unsigned n = s->room;
    if (came_online) {
        printf("[HUB] room %u ONLINE\n", n);
        schedule_log("ROOM,%u,ONLINE", n);
    }
    if (changed) {
        printf("[HUB] room %u %s, light %s\n", n,
               s->occupied ? "OCCUPIED" : "EMPTY", s->light_on ? "ON" : "OFF");
        schedule_log("ROOM,%u,%s,light %s", n,
                     s->occupied ? "OCCUPIED" : "EMPTY", s->light_on ? "ON" : "OFF");
    }
}

static void handle_entrance(const entrance_event_t *ev)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_inside = ev->inside_count;
    xSemaphoreGive(s_lock);
    if (ev->granted) {
        printf("[HUB] building occupancy: %d\n", ev->inside_count);
    }
}

/* A room that has not reported for HUB_OFFLINE_MS is marked OFFLINE (logged once). */
static void check_offline(void)
{
    uint32_t now = now_ms();
    for (int i = 0; i < ROOM_COUNT; i++) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool went_offline = s_rooms[i].online && now - s_rooms[i].last_seen_ms > HUB_OFFLINE_MS;
        if (went_offline) s_rooms[i].online = false;
        xSemaphoreGive(s_lock);

        if (went_offline) {
            printf("[HUB] room %d OFFLINE (no report for %d s)\n", i + 1, HUB_OFFLINE_MS / 1000);
            schedule_log("ROOM,%d,OFFLINE", i + 1);
        }
    }
}

static void hub_task(void *arg)
{
    hub_msg_t m;
    for (;;) {
        /* wait at most 1 s so the offline check runs even when nothing arrives */
        if (xQueueReceive(s_queue, &m, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (m.type == HUB_MSG_ROOM) {
                handle_room(&m.room);
            } else {
                handle_entrance(&m.entrance);
            }
        }
        check_offline();
    }
}

bool hub_get_room(uint8_t room, hub_room_t *out)
{
    if (room < 1 || room > ROOM_COUNT || s_lock == NULL) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_rooms[room - 1];
    xSemaphoreGive(s_lock);
    return true;
}

/* ---- console commands ---- */

static void cmd_rooms(const char *args)
{
    uint32_t now = now_ms();
    for (uint8_t i = 1; i <= ROOM_COUNT; i++) {
        hub_room_t r;
        hub_get_room(i, &r);
        if (!r.ever_seen) {
            printf("Room %u: no report yet\n", i);
            continue;
        }
        printf("Room %u: %-7s %-8s light %-3s light-on %lu s, last report %lu s ago\n", i,
               r.online ? "ONLINE" : "OFFLINE",
               r.status.occupied ? "OCCUPIED" : "empty",
               r.status.light_on ? "ON" : "off",
               (unsigned long)r.status.light_on_s,
               (unsigned long)((now - r.last_seen_ms) / 1000));
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int inside = s_inside;
    xSemaphoreGive(s_lock);
    printf("Building: %d people inside\n", inside);
}

static void cmd_roomfail(const char *args)
{
    if (!auth_is_logged_in()) {
        printf("login required: login <pin>\n");
        return;
    }
    int room;
    char onoff[8] = "";
    if (sscanf(args, "%d %7s", &room, onoff) != 2 || room < 1 || room > ROOM_COUNT ||
        (strcmp(onoff, "on") != 0 && strcmp(onoff, "off") != 0)) {
        printf("usage: roomfail <1|2> on|off\n");
        return;
    }
    bool on = (strcmp(onoff, "on") == 0);
    room_control_simulate_fault((uint8_t)room, on);
    schedule_log("TEST,room %d fault %s", room, onoff);
    printf("room %d %s\n", room, on ? "stopped reporting (OFFLINE after 15 s)" : "reporting again");
}

esp_err_t hub_start(void)
{
    s_queue = xQueueCreate(HUB_QUEUE_LEN, sizeof(hub_msg_t));
    s_lock  = xSemaphoreCreateMutex();
    if (s_queue == NULL || s_lock == NULL) return ESP_ERR_NO_MEM;

    room_control_set_publish_cb(on_room_status);
    entrance_set_event_cb(on_entrance_event);

    esp_err_t e = entrance_register_command("rooms",
        "rooms               room occupancy, lights, online/offline, people inside", cmd_rooms);
    if (e == ESP_OK) {
        e = entrance_register_command("roomfail",
            "roomfail <n> on|off [login] test: room n stops reporting (OFFLINE after 15 s)",
            cmd_roomfail);
    }
    if (e != ESP_OK) return e;

    if (xTaskCreate(hub_task, "hub", HUB_STACK, NULL, 6, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
