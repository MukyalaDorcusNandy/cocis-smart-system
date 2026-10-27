/*
 * entrance_node.c
 * CoCIS Smart Control and Monitoring System -- ENTRANCE TERMINAL (master)
 * Owner: Dorcus Mukyala
 *
 * WHAT THIS NODE DOES
 *   1. Receives RFID scans at TWO scan points: ENTRY and EXIT
 *   2. Decides GRANT or DENY (registered? blocked? schedule? anti-passback?)
 *   3. Drives the door (servo), indicator LEDs and a buzzer
 *   4. Keeps the "who is inside" record and an event log
 *
 * RFID IS SIMULATED: Wokwi has no ready-made RFID reader, so a scan is
 * typed in the serial monitor:   in A1B2C3D4    or    out A1B2C3D4
 * A real build would replace ONLY console_task() with an MFRC522 reader
 * that pushes the same scan_event_t into scan_queue.
 *
 * TASK / DATA FLOW
 *
 *   console_task --(scan_event_t)--> scan_queue --> access_task
 *                                                      |  (decides, uses state_mutex)
 *                                                      +--(actuator_cmd_t)--> actuator_queue
 *                                                                                  |
 *                                                                           actuator_task
 *                                                                      (servo, LEDs, buzzer)
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"

/* ------------------------------------------------------------------ */
/* 1. CONFIGURATION                                                    */
/* ------------------------------------------------------------------ */
#define UID_LEN         16      /* tag id text, e.g. "A1B2C3D4"          */
#define NAME_LEN        24
#define DOOR_OPEN_MS    3000    /* how long a door stays open            */
#define LOG_RING_SIZE   20      /* last N events kept in RAM             */
#define LOG_LINE_LEN    160

/* GPIO pins (must match diagram.json) */
#define PIN_ENTRY_SERVO 18
#define PIN_EXIT_SERVO  19
#define PIN_LED_GRANT   25
#define PIN_LED_DENY    26
#define PIN_BUZZER      27

/* ------------------------------------------------------------------ */
/* 2. DATA TYPES                                                       */
/* ------------------------------------------------------------------ */
typedef enum { POINT_ENTRY = 0, POINT_EXIT = 1 } scan_point_t;

/* One RFID scan, produced by the reader, consumed by access_task */
typedef struct {
    scan_point_t point;
    char uid[UID_LEN];
} scan_event_t;

/* One command for the door / indicators */
typedef struct {
    scan_point_t door;
    bool granted;
} actuator_cmd_t;

/* One registered person */
typedef struct {
    char uid[UID_LEN];
    char name[NAME_LEN];
    bool blocked;   /* lost / stolen card */
    bool inside;    /* currently inside the building? */
} user_t;

/* ------------------------------------------------------------------ */
/* 3. SHARED STATE (always protect with state_mutex)                   */
/* ------------------------------------------------------------------ */
/* TODO(storage owner): load this table from users.csv on the SD card  */
static user_t users[] = {
    { "A1B2C3D4", "Alice (Staff)",   false, false },
    { "B2C3D4E5", "Brian (Student)", false, false },
    { "C3D4E5F6", "Carol (Student)", false, false },
    { "D4E5F6A7", "Dan (Admin)",     false, false },
    { "DEADBEEF", "Lost Card",       true,  false },
};
#define NUM_USERS ((int)(sizeof(users) / sizeof(users[0])))

static int  inside_count = 0;

/* access schedule, in minutes after midnight (default 07:00 - 20:00) */
/* TODO(schedule owner): replace the "sched" command with the login-protected one */
static int  open_min  = 7 * 60;
static int  close_min = 20 * 60;
static bool emergency = false;      /* emergency egress: exit allowed any time */

/* simulated clock (no RTC in Wokwi): base time + time elapsed since */
static int     clock_base_sec = 8 * 3600;   /* starts at 08:00:00 */
static int64_t clock_base_us  = 0;

/* RAM ring buffer of the last events (CSV lines) */
static char log_ring[LOG_RING_SIZE][LOG_LINE_LEN];
static int  log_head  = 0;
static int  log_count = 0;

static SemaphoreHandle_t state_mutex;   /* guards everything above */
static QueueHandle_t     scan_queue;    /* scans -> access_task    */
static QueueHandle_t     actuator_queue;/* decisions -> actuators  */

/* ------------------------------------------------------------------ */
/* 4. SMALL HELPERS                                                    */
/* ------------------------------------------------------------------ */

/* current time of day in seconds (call with state_mutex held) */
static int now_sec(void)
{
    int64_t elapsed = (esp_timer_get_time() - clock_base_us) / 1000000;
    return (int)((clock_base_sec + elapsed) % 86400);
}

/* is the building open at this time? (handles overnight windows too) */
static bool schedule_open_locked(int sec)
{
    int m = sec / 60;
    if (open_min <= close_min) {
        return (m >= open_min) && (m < close_min);
    }
    return (m >= open_min) || (m < close_min);
}

static user_t *find_user(const char *uid)
{
    for (int i = 0; i < NUM_USERS; i++) {
        if (strcmp(users[i].uid, uid) == 0) {
            return &users[i];
        }
    }
    return NULL;
}

/* store one CSV line in the ring buffer (call with state_mutex held) */
static void log_push_locked(const char *line)
{
    int idx = (log_head + log_count) % LOG_RING_SIZE;
    if (log_count == LOG_RING_SIZE) {
        idx = log_head;                          /* overwrite oldest */
        log_head = (log_head + 1) % LOG_RING_SIZE;
    } else {
        log_count++;
    }
    snprintf(log_ring[idx], LOG_LINE_LEN, "%s", line);
}

/* ------------------------------------------------------------------ */
/* 5. HARDWARE: servo (door), LEDs, buzzer                             */
/* ------------------------------------------------------------------ */
static void gpio_out_init(int pin)
{
    ESP_ERROR_CHECK(gpio_reset_pin(pin));
    ESP_ERROR_CHECK(gpio_set_direction(pin, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(pin, 0));
}

/* A hobby servo expects a 50 Hz signal; pulse width 0.5-2.4 ms = 0-180 deg */
static void servo_set_angle(ledc_channel_t ch, int angle)
{
    uint32_t pulse_us = 500 + (uint32_t)angle * 1900 / 180;
    uint32_t duty = pulse_us * ((1u << 14) - 1) / 20000;   /* 20 ms period */
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, ch, duty));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, ch));
}

static void hardware_init(void)
{
    gpio_out_init(PIN_LED_GRANT);
    gpio_out_init(PIN_LED_DENY);
    gpio_out_init(PIN_BUZZER);

    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_14_BIT,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = 50,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    ledc_channel_config_t entry_ch = {
        .gpio_num   = PIN_ENTRY_SERVO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&entry_ch));

    ledc_channel_config_t exit_ch = entry_ch;
    exit_ch.gpio_num = PIN_EXIT_SERVO;
    exit_ch.channel  = LEDC_CHANNEL_1;
    ESP_ERROR_CHECK(ledc_channel_config(&exit_ch));

    /* both doors start closed (0 degrees) */
    servo_set_angle(LEDC_CHANNEL_0, 0);
    servo_set_angle(LEDC_CHANNEL_1, 0);
}

/* crude buzzer: toggle the pin at about 1 kHz for 'ms' milliseconds */
static void beep(int ms)
{
    for (int i = 0; i < ms; i++) {
        gpio_set_level(PIN_BUZZER, 1);
        esp_rom_delay_us(500);
        gpio_set_level(PIN_BUZZER, 0);
        esp_rom_delay_us(500);
    }
}

/* ------------------------------------------------------------------ */
/* 6. THE ACCESS DECISION  (the heart of the entrance node)            */
/* ------------------------------------------------------------------ */
static void process_scan(const scan_event_t *ev)
{
    const char *point_txt = (ev->point == POINT_ENTRY) ? "ENTRY" : "EXIT";
    const char *reason    = "ok";
    char name[NAME_LEN]   = "-";
    bool granted          = false;
    char line[LOG_LINE_LEN];
    int  count_after;

    xSemaphoreTake(state_mutex, portMAX_DELAY);          /* ---- lock ---- */

    int  now      = now_sec();
    bool open_now = schedule_open_locked(now);
    user_t *u     = find_user(ev->uid);

    if (u == NULL) {
        reason = "unknown tag";
    } else {
        snprintf(name, sizeof(name), "%s", u->name);

        if (u->blocked) {
            reason = "tag blocked";
        } else if (ev->point == POINT_ENTRY && u->inside) {
            reason = "already inside (anti-passback)";
        } else if (ev->point == POINT_EXIT && !u->inside) {
            reason = "not inside";
        } else if (!open_now && !(ev->point == POINT_EXIT && emergency)) {
            reason = "outside access hours";
        } else {
            granted = true;
            reason  = open_now ? "ok" : "emergency exit";
        }
    }

    if (granted) {
        u->inside = (ev->point == POINT_ENTRY);
        inside_count += (ev->point == POINT_ENTRY) ? 1 : -1;
    }
    count_after = inside_count;

    /* CSV record: time,uid,name,point,result,reason   <- storage owner writes this to SD */
    snprintf(line, sizeof(line), "%02d:%02d:%02d,%s,%s,%s,%s,%s",
             now / 3600, (now / 60) % 60, now % 60,
             ev->uid, name, point_txt,
             granted ? "GRANTED" : "DENIED", reason);
    log_push_locked(line);

    xSemaphoreGive(state_mutex);                         /* --- unlock --- */

    printf(">> %s %s (%s) -> %s [%s] | inside now: %d\n",
           point_txt, ev->uid, name, granted ? "GRANTED" : "DENIED", reason, count_after);

    /* TODO(comm owner): publish this event to the room nodes / dashboard over Wi-Fi */
    /* TODO(storage owner): append 'line' to access_log.csv on the SD card */

    actuator_cmd_t cmd = { .door = ev->point, .granted = granted };
    xQueueSend(actuator_queue, &cmd, 0);
}

/* ------------------------------------------------------------------ */
/* 7. TASKS                                                            */
/* ------------------------------------------------------------------ */

/* Takes scans one at a time from the queue -> two scans can never be
 * processed at the same instant, so the records cannot get mixed up. */
static void access_task(void *arg)
{
    scan_event_t ev;
    while (1) {
        if (xQueueReceive(scan_queue, &ev, portMAX_DELAY) == pdTRUE) {
            process_scan(&ev);
        }
    }
}

/* Moves the servo and flashes LEDs. Runs in its own task so that the 3 s
 * the door stays open never blocks the decision logic. */
static void actuator_task(void *arg)
{
    actuator_cmd_t cmd;
    while (1) {
        if (xQueueReceive(actuator_queue, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (cmd.granted) {
            ledc_channel_t ch = (cmd.door == POINT_ENTRY) ? LEDC_CHANNEL_0 : LEDC_CHANNEL_1;
            gpio_set_level(PIN_LED_GRANT, 1);
            servo_set_angle(ch, 90);                       /* open  */
            vTaskDelay(pdMS_TO_TICKS(DOOR_OPEN_MS));
            servo_set_angle(ch, 0);                        /* close */
            gpio_set_level(PIN_LED_GRANT, 0);
        } else {
            gpio_set_level(PIN_LED_DENY, 1);
            beep(150);
            vTaskDelay(pdMS_TO_TICKS(100));
            beep(150);
            vTaskDelay(pdMS_TO_TICKS(1500));              /* red LED stays on long enough to see */
            gpio_set_level(PIN_LED_DENY, 0);
        }
    }
}

/* ------------------------------------------------------------------ */
/* 8. SERIAL COMMANDS (simulated RFID reader + test/management commands) */
/* ------------------------------------------------------------------ */
static void print_help(void)
{
    printf("\nEntrance node commands:\n"
           "  in <UID>            simulate a tag scan at the ENTRY reader\n"
           "  out <UID>           simulate a tag scan at the EXIT reader\n"
           "  inside              who is inside the building now\n"
           "  users               list registered users\n"
           "  log                 last access events (CSV)\n"
           "  status              time, schedule, emergency mode, count\n"
           "  time HH:MM          set the simulated clock\n"
           "  sched HH:MM HH:MM   set open and close time (temporary, no login yet)\n"
           "  emergency on|off    allow exit outside hours\n"
           "  help\n"
           "Test UIDs: A1B2C3D4 B2C3D4E5 C3D4E5F6 D4E5F6A7 DEADBEEF(blocked)\n\n");
}

static bool parse_hhmm(const char *s, int *minutes)
{
    int h, m;
    if (sscanf(s, "%d:%d", &h, &m) != 2) return false;
    if (h < 0 || h > 23 || m < 0 || m > 59) return false;
    *minutes = h * 60 + m;
    return true;
}

static void cmd_inside(void)
{
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    printf("People inside: %d\n", inside_count);
    for (int i = 0; i < NUM_USERS; i++) {
        if (users[i].inside) {
            printf("  %s  %s\n", users[i].uid, users[i].name);
        }
    }
    xSemaphoreGive(state_mutex);
}

static void cmd_users(void)
{
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    for (int i = 0; i < NUM_USERS; i++) {
        printf("  %s  %-16s %s%s\n", users[i].uid, users[i].name,
               users[i].inside ? "[inside] " : "[outside]",
               users[i].blocked ? " [BLOCKED]" : "");
    }
    xSemaphoreGive(state_mutex);
}

static void cmd_log(void)
{
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    printf("time,uid,name,point,result,reason\n");
    for (int i = 0; i < log_count; i++) {
        printf("%s\n", log_ring[(log_head + i) % LOG_RING_SIZE]);
    }
    xSemaphoreGive(state_mutex);
}

static void cmd_status(void)
{
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    int now = now_sec();
    printf("Time %02d:%02d:%02d | open %02d:%02d - %02d:%02d (%s) | emergency %s | inside %d\n",
           now / 3600, (now / 60) % 60, now % 60,
           open_min / 60, open_min % 60, close_min / 60, close_min % 60,
           schedule_open_locked(now) ? "OPEN" : "CLOSED",
           emergency ? "ON" : "off", inside_count);
    xSemaphoreGive(state_mutex);
}

static void handle_command(char *line)
{
    char cmd[16] = "", a1[16] = "", a2[16] = "";
    int n = sscanf(line, "%15s %15s %15s", cmd, a1, a2);
    if (n <= 0) return;

    for (char *p = cmd; *p; p++) *p = (char)tolower((unsigned char)*p);

    if (strcmp(cmd, "in") == 0 || strcmp(cmd, "out") == 0) {
        if (n < 2) { printf("usage: %s <UID>\n", cmd); return; }
        scan_event_t ev;
        ev.point = (cmd[0] == 'i') ? POINT_ENTRY : POINT_EXIT;
        int i = 0;
        for (; a1[i] && i < UID_LEN - 1; i++) ev.uid[i] = (char)toupper((unsigned char)a1[i]);
        ev.uid[i] = '\0';
        if (xQueueSend(scan_queue, &ev, 0) != pdTRUE) {
            printf("scan queue full, try again\n");
        }
    } else if (strcmp(cmd, "inside") == 0) {
        cmd_inside();
    } else if (strcmp(cmd, "users") == 0) {
        cmd_users();
    } else if (strcmp(cmd, "log") == 0) {
        cmd_log();
    } else if (strcmp(cmd, "status") == 0) {
        cmd_status();
    } else if (strcmp(cmd, "time") == 0) {
        int mins;
        if (n < 2 || !parse_hhmm(a1, &mins)) { printf("usage: time HH:MM\n"); return; }
        xSemaphoreTake(state_mutex, portMAX_DELAY);
        clock_base_sec = mins * 60;
        clock_base_us  = esp_timer_get_time();
        xSemaphoreGive(state_mutex);
        cmd_status();
    } else if (strcmp(cmd, "sched") == 0) {
        int o, c;
        if (n < 3 || !parse_hhmm(a1, &o) || !parse_hhmm(a2, &c)) {
            printf("usage: sched HH:MM HH:MM\n");
            return;
        }
        xSemaphoreTake(state_mutex, portMAX_DELAY);
        open_min = o;
        close_min = c;
        xSemaphoreGive(state_mutex);
        cmd_status();
    } else if (strcmp(cmd, "emergency") == 0) {
        if (n < 2) { printf("usage: emergency on|off\n"); return; }
        xSemaphoreTake(state_mutex, portMAX_DELAY);
        emergency = (strcmp(a1, "on") == 0);
        xSemaphoreGive(state_mutex);
        cmd_status();
    } else if (strcmp(cmd, "help") == 0) {
        print_help();
    } else {
        printf("unknown command '%s' (type help)\n", cmd);
    }
}

/* Reads typed characters from the serial port, builds a line, runs it.
 * In a real build, replace this with an MFRC522 reader task that fills
 * a scan_event_t and calls xQueueSend(scan_queue, ...). */
static void console_task(void *arg)
{
    char line[64];
    int len = 0;
    uint8_t c;

    while (1) {
        int n = uart_read_bytes(UART_NUM_0, &c, 1, pdMS_TO_TICKS(100));
        if (n <= 0) continue;
        if (c == '\r' || c == '\n') {
            if (len > 0) {
                uart_write_bytes(UART_NUM_0, "\r\n", 2);   /* move to a new line */
                line[len] = '\0';
                handle_command(line);
                len = 0;
            }
        } else if (c == 0x08 || c == 0x7F) {                /* backspace */
            if (len > 0) {
                len--;
                uart_write_bytes(UART_NUM_0, "\b \b", 3);   /* erase it on screen */
            }
        } else if (len < (int)sizeof(line) - 1) {
            line[len++] = (char)c;
            uart_write_bytes(UART_NUM_0, &c, 1);            /* echo it back */
        }
    }
}

/* ------------------------------------------------------------------ */
/* 9. START-UP                                                         */
/* ------------------------------------------------------------------ */
void app_main(void)
{
    state_mutex    = xSemaphoreCreateMutex();
    scan_queue     = xQueueCreate(8, sizeof(scan_event_t));
    actuator_queue = xQueueCreate(8, sizeof(actuator_cmd_t));
    clock_base_us  = esp_timer_get_time();

    hardware_init();
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0));

    xTaskCreate(access_task,   "access",   4096, NULL, 5, NULL);
    xTaskCreate(actuator_task, "actuator", 4096, NULL, 4, NULL);
    xTaskCreate(console_task,  "console",  4096, NULL, 3, NULL);

    printf("\n=== CoCIS ENTRANCE NODE ready ===\n");
    print_help();
    cmd_status();
}