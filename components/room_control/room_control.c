#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "room_control.h"
#include "room_config.h"

typedef struct {
    uint8_t           room;
    gpio_num_t        pir, led;
    SemaphoreHandle_t lock;       /* guards st and fault */
    room_status_t     st;
    bool              fault;      /* test: reports are not sent */
    bool              started;
} room_ctx_t;

static room_ctx_t        s_ctx[ROOM_COUNT];
static room_publish_cb_t s_cb;

void room_control_set_publish_cb(room_publish_cb_t cb)
{
    s_cb = cb;
}

static void publish(room_ctx_t *c)
{
    room_status_t snap;
    xSemaphoreTake(c->lock, portMAX_DELAY);
    bool fault = c->fault;
    if (!fault) {
        c->st.seq++;
        snap = c->st;
    }
    xSemaphoreGive(c->lock);
    if (!fault && s_cb) s_cb(&snap);
}

void room_control_simulate_fault(uint8_t room, bool fault)
{
    if (room < 1 || room > ROOM_COUNT || !s_ctx[room - 1].started) return;
    room_ctx_t *c = &s_ctx[room - 1];
    xSemaphoreTake(c->lock, portMAX_DELAY);
    c->fault = fault;
    xSemaphoreGive(c->lock);
}

static void room_task(void *arg)
{
    room_ctx_t *c = (room_ctx_t *)arg;
    int64_t last_motion = 0;
    int64_t last_tick = esp_timer_get_time() / 1000;
    int64_t last_pub  = last_tick;
    uint32_t rem_ms = 0;

    publish(c);                                   /* announce at boot */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(ROOM_POLL_MS));
        int64_t now = esp_timer_get_time() / 1000;
        bool motion = gpio_get_level(c->pir);
        bool changed = false, occ, light;

        xSemaphoreTake(c->lock, portMAX_DELAY);
        if (motion) {
            last_motion = now;
            if (!c->st.occupied) { c->st.occupied = true; changed = true; }
        } else if (c->st.occupied && now - last_motion > ROOM_VACANCY_DELAY_MS) {
            c->st.occupied = false;
            changed = true;
        }
        if (c->st.light_on != c->st.occupied) {      /* light follows occupancy */
            c->st.light_on = c->st.occupied;
            gpio_set_level(c->led, c->st.light_on);
            changed = true;
        }
        if (c->st.light_on) {                         /* energy accounting */
            rem_ms += (uint32_t)(now - last_tick);
            c->st.light_on_s += rem_ms / 1000;
            rem_ms %= 1000;
        }
        occ = c->st.occupied;
        light = c->st.light_on;
        xSemaphoreGive(c->lock);
        last_tick = now;

        if (changed)
            printf("[ROOM %u] %s, light %s\n", c->room, occ ? "OCCUPIED" : "EMPTY", light ? "ON" : "OFF");
        if (changed || now - last_pub >= ROOM_HEARTBEAT_MS) {
            publish(c);
            last_pub = now;
        }
    }
}

esp_err_t room_control_start(uint8_t room)
{
    if (room < 1 || room > ROOM_COUNT) return ESP_ERR_INVALID_ARG;
    room_ctx_t *c = &s_ctx[room - 1];
    if (c->started) return ESP_ERR_INVALID_STATE;

    c->room = room;
    c->st.room = room;
    c->pir = (gpio_num_t)ROOM_PIR_GPIO(room);
    c->led = (gpio_num_t)ROOM_LED_GPIO(room);
    c->lock = xSemaphoreCreateMutex();
    if (!c->lock) return ESP_ERR_NO_MEM;

    gpio_config_t in = {
        .pin_bit_mask = 1ULL << c->pir,
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
    };
    gpio_config_t out = {
        .pin_bit_mask = 1ULL << c->led,
        .mode = GPIO_MODE_OUTPUT,
    };
    esp_err_t e = gpio_config(&in);
    if (e == ESP_OK) e = gpio_config(&out);
    if (e == ESP_OK) e = gpio_set_level(c->led, 0);
    if (e != ESP_OK) {
        vSemaphoreDelete(c->lock);
        return e;
    }

    char name[12];
    snprintf(name, sizeof(name), "room%u", (unsigned)room);
    if (xTaskCreate(room_task, name, 4096, c, 5, NULL) != pdPASS) {
        vSemaphoreDelete(c->lock);
        return ESP_ERR_NO_MEM;
    }
    c->started = true;
    return ESP_OK;
}

bool room_control_get(uint8_t room, room_status_t *out)
{
    if (room < 1 || room > ROOM_COUNT || !s_ctx[room - 1].started) return false;
    room_ctx_t *c = &s_ctx[room - 1];
    xSemaphoreTake(c->lock, portMAX_DELAY);
    *out = c->st;
    xSemaphoreGive(c->lock);
    return true;
}
