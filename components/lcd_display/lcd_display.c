/*
 * lcd_display.c - 16x2 LCD at the building entrance.
 *
 * Hardware: Wokwi "wokwi-lcd1602" in I2C mode. The I2C backpack (PCF8574)
 * turns one I2C byte into 8 output pins:
 *     bit 0 RS (0 = command, 1 = character)   bit 2 EN (pulse = "take it")
 *     bit 3 backlight                         bits 4-7 = 4 data lines D4-D7
 * So each byte for the LCD is sent as two 4-bit halves (4-bit mode).
 * I2C pins: SDA GPIO 15, SCL GPIO 2 (any ESP32 pin can be I2C).
 *
 * Flow:
 *   access_task --event--> on_entrance_event() --lcd_queue--> lcd_task --> LCD
 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "access_control.h"
#include "schedule.h"
#include "lcd_display.h"

/* ---- pins and settings (must match diagram.json) ---- */
#define PIN_SDA         15
#define PIN_SCL         2
#define LCD_ADDR        0x27
#define SHOW_EVENT_MS   3000    /* how long a scan result stays on screen */

/* PCF8574 bits */
#define BIT_RS          0x01
#define BIT_EN          0x04
#define BIT_BACKLIGHT   0x08

static i2c_master_dev_handle_t lcd_dev;
static QueueHandle_t           lcd_queue;   /* scan decisions -> lcd_task */

/* ------------------------------------------------------------------ */
/* 1. LOW LEVEL: send one byte to the LCD as two 4-bit halves          */
/* ------------------------------------------------------------------ */
static void lcd_send(uint8_t value, uint8_t rs)
{
    uint8_t hi = (value & 0xF0) | rs | BIT_BACKLIGHT;
    uint8_t lo = ((value << 4) & 0xF0) | rs | BIT_BACKLIGHT;
    /* each half: EN high then EN low; the LCD reads the data on the falling edge */
    uint8_t buf[4] = { hi | BIT_EN, hi, lo | BIT_EN, lo };
    i2c_master_transmit(lcd_dev, buf, sizeof(buf), 50);
}

static void lcd_cmd(uint8_t cmd)
{
    lcd_send(cmd, 0);
    vTaskDelay(pdMS_TO_TICKS(2));        /* clear/home need about 1.5 ms */
}

/* Writes one full line (row 0 or 1), padded with spaces to 16 characters */
static void lcd_line(int row, const char *text)
{
    lcd_cmd(row == 0 ? 0x80 : 0xC0);     /* move the cursor to the line start */
    int i = 0;
    for (; text[i] && i < 16; i++) lcd_send((uint8_t)text[i], BIT_RS);
    for (; i < 16; i++) lcd_send(' ', BIT_RS);
}

static void lcd_init(void)
{
    vTaskDelay(pdMS_TO_TICKS(50));
    /* the standard HD44780 wake-up: 3 x "8-bit mode", then "4-bit mode" */
    uint8_t wake[] = { 0x30, 0x30, 0x30, 0x20 };
    for (int i = 0; i < 4; i++) {
        uint8_t b = wake[i] | BIT_BACKLIGHT;
        uint8_t buf[2] = { b | BIT_EN, b };
        i2c_master_transmit(lcd_dev, buf, 2, 50);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    lcd_cmd(0x28);   /* 4-bit, 2 lines, 5x8 font */
    lcd_cmd(0x0C);   /* display on, cursor off */
    lcd_cmd(0x06);   /* move right after each character */
    lcd_cmd(0x01);   /* clear */
}

/* ------------------------------------------------------------------ */
/* 2. WHAT TO SHOW                                                     */
/* ------------------------------------------------------------------ */

/* First name only: "Brian (Student)" -> "Brian" */
static void first_name(const char *name, char *out, int len)
{
    int i = 0;
    for (; name[i] && name[i] != ' ' && i < len - 1; i++) out[i] = name[i];
    out[i] = '\0';
}

/* Short reason that fits on 16 characters */
static const char *short_reason(const char *reason)
{
    if (strstr(reason, "anti-passback")) return "Already inside";
    if (strstr(reason, "outside"))       return "Building closed";
    if (strstr(reason, "unknown"))       return "Unknown card";
    if (strstr(reason, "blocked"))       return "Card blocked";
    if (strstr(reason, "not inside"))    return "Not inside";
    return reason;
}

static void show_event(const entrance_event_t *ev)
{
    char l1[40], l2[40], first[12];   /* lcd_line() cuts each line to 16 */
    first_name(ev->name, first, sizeof(first));
    if (ev->granted) {
        snprintf(l1, sizeof(l1), "%s %s", ev->is_entry ? "WELCOME" : "BYE", first);
        /* e.g. "Student BLUE" - the card colour in capitals */
        char color[8];
        snprintf(color, sizeof(color), "%s", entrance_role_color(ev->role));
        for (char *p = color; *p; p++) *p = (char)toupper((unsigned char)*p);
        snprintf(l2, sizeof(l2), "%s %s card", entrance_role_name(ev->role), color);
    } else {
        snprintf(l1, sizeof(l1), "DENIED %s", ev->known ? first : "");
        snprintf(l2, sizeof(l2), "%s", short_reason(ev->reason));
    }
    lcd_line(0, l1);
    lcd_line(1, l2);
}

static void show_idle(void)
{
    char now[16], l1[40], l2[40];
    clock_str(now, sizeof(now));
    snprintf(l1, sizeof(l1), "CoCIS  %s", now);
    snprintf(l2, sizeof(l2), "In:%-3d %s", entrance_inside_count(),
             schedule_is_open() ? "OPEN" : "CLOSED");
    lcd_line(0, l1);
    lcd_line(1, l2);
}

/* ------------------------------------------------------------------ */
/* 3. TASK AND START-UP                                                */
/* ------------------------------------------------------------------ */

/* Entrance listener: runs in the access task, so it only copies the event
 * into the queue (no waiting). */
static void on_entrance_event(const entrance_event_t *ev)
{
    xQueueSend(lcd_queue, ev, 0);
}

static void lcd_task(void *arg)
{
    (void)arg;
    lcd_init();
    lcd_line(0, "CoCIS Entrance");
    lcd_line(1, "Tap your card");
    vTaskDelay(pdMS_TO_TICKS(1500));

    entrance_event_t ev;
    while (1) {
        /* wait up to 1 s for a scan; if none, refresh the clock screen */
        if (xQueueReceive(lcd_queue, &ev, pdMS_TO_TICKS(1000)) == pdTRUE) {
            show_event(&ev);
            vTaskDelay(pdMS_TO_TICKS(SHOW_EVENT_MS));
        } else {
            show_idle();
        }
    }
}

esp_err_t lcd_display_start(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_SDA,
        .scl_io_num = PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    esp_err_t e = i2c_new_master_bus(&bus_cfg, &bus);
    if (e != ESP_OK) return e;

    /* is an LCD there? If not, carry on without it */
    if (i2c_master_probe(bus, LCD_ADDR, 50) != ESP_OK) {
        printf("[LCD] no LCD at 0x%02X, running without it\n", LCD_ADDR);
        return ESP_OK;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = LCD_ADDR,
        .scl_speed_hz = 100000,
    };
    e = i2c_master_bus_add_device(bus, &dev_cfg, &lcd_dev);
    if (e != ESP_OK) return e;

    lcd_queue = xQueueCreate(4, sizeof(entrance_event_t));
    if (lcd_queue == NULL) return ESP_ERR_NO_MEM;
    entrance_set_event_cb(on_entrance_event);

    if (xTaskCreate(lcd_task, "lcd", 3072, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    printf("[LCD] 16x2 LCD ready at 0x%02X (SDA %d, SCL %d)\n", LCD_ADDR, PIN_SDA, PIN_SCL);
    return ESP_OK;
}
