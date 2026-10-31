/*
 * access_control.c
 * CoCIS Smart Control and Monitoring System -- ENTRANCE / ACCESS CONTROL component
 * Owner: Dorcus Mukyala
 *
 * Runs inside the ONE shared ESP32 firmware (cocis_system). It does not have
 * its own app_main(): cocis_system/main/main.c calls entrance_start().
 *
 * WHAT THIS NODE DOES
 *   1. Receives RFID scans at TWO scan points: ENTRY and EXIT
 *   2. Decides GRANT or DENY (registered? blocked? anti-passback? opening
 *      hours from the schedule component? emergency exit?)
 *   3. Drives the door (servo), indicator LEDs and a buzzer
 *   4. Keeps the "who is inside" record and a log of recent decisions;
 *      every decision is also written to the system log (schedule_log)
 *   5. Runs the serial console; other components add commands to it
 *
 * RFID IS SIMULATED: Wokwi has no ready-made RFID reader, so a scan is
 * typed in the serial monitor:   in A1B2C3D4    or    out A1B2C3D4
 * A real build would add an MFRC522 reader task that pushes the same
 * scan_event_t into scan_queue; access_task would not change.
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
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_timer.h"

#include "access_control.h"
#include "schedule.h"
#include "storage.h"

/* ------------------------------------------------------------------ */
/* 1. CONFIGURATION                                                    */
/* ------------------------------------------------------------------ */
#define UID_LEN         ENTRANCE_UID_LEN    /* tag id text, e.g. "A1B2C3D4"      */
#define NAME_LEN        ENTRANCE_NAME_LEN
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
/* Registered cards. The table is fixed in code; only the "blocked" flags
 * can be changed (block/unblock) and are saved in NVS as a bit mask. */
#define BLOCKED_KEY "blocked"
static user_t users[] = {
    { "A1B2C3D4", "Alice (Staff)",   false, false },
    { "B2C3D4E5", "Brian (Student)", false, false },
    { "C3D4E5F6", "Carol (Student)", false, false },
    { "D4E5F6A7", "Dan (Admin)",     false, false },
    { "DEADBEEF", "Lost Card",       true,  false },
};
#define NUM_USERS ((int)(sizeof(users) / sizeof(users[0])))
_Static_assert(NUM_USERS <= 32, "blocked flags are saved as a 32-bit mask");

static int  inside_count = 0;

/* Opening hours and the clock live in the schedule component (schedule.h). */
static bool emergency = false;      /* emergency egress: exit allowed any time */

/* RAM ring buffer of the last events (CSV lines) */
static char log_ring[LOG_RING_SIZE][LOG_LINE_LEN];
static int  log_head  = 0;
static int  log_count = 0;

static SemaphoreHandle_t state_mutex;   /* guards everything above */
static QueueHandle_t     scan_queue;    /* scans -> access_task    */
static QueueHandle_t     actuator_queue;/* decisions -> actuators  */

/* Hooks for the other team members (see access_control.h) */
static entrance_event_cb_t event_cb = NULL;     /* told about every decision */

#define MAX_EXT_CMDS 16
typedef struct {
    const char          *name;      /* lower-case command word */
    const char          *help;      /* one line shown by "help" */
    entrance_cmd_fn_t    fn;
} ext_cmd_t;
static ext_cmd_t ext_cmds[MAX_EXT_CMDS];
static int       ext_cmd_count = 0;

void entrance_set_event_cb(entrance_event_cb_t cb)
{
    event_cb = cb;
}

/* Commands handled inside this file (see handle_command) */
static const char *const builtin_cmds[] = { "in", "out", "inside", "users", "log", "status", "help" };
#define NUM_BUILTIN ((int)(sizeof(builtin_cmds) / sizeof(builtin_cmds[0])))

esp_err_t entrance_register_command(const char *name, const char *help,
                                    entrance_cmd_fn_t fn)
{
    if (name == NULL || fn == NULL) return ESP_ERR_INVALID_ARG;
    if (ext_cmd_count >= MAX_EXT_CMDS) return ESP_ERR_NO_MEM;
    /* every command word must exist only once */
    for (int i = 0; i < NUM_BUILTIN; i++) {
        if (strcmp(name, builtin_cmds[i]) == 0) return ESP_ERR_INVALID_STATE;
    }
    for (int i = 0; i < ext_cmd_count; i++) {
        if (strcmp(name, ext_cmds[i].name) == 0) return ESP_ERR_INVALID_STATE;
    }
    ext_cmds[ext_cmd_count].name = name;
    ext_cmds[ext_cmd_count].help = (help != NULL) ? help : "";
    ext_cmds[ext_cmd_count].fn   = fn;
    ext_cmd_count++;
    return ESP_OK;
}

int entrance_inside_count(void)
{
    int n = 0;
    if (state_mutex != NULL) {
        xSemaphoreTake(state_mutex, portMAX_DELAY);
        n = inside_count;
        xSemaphoreGive(state_mutex);
    }
    return n;
}

void entrance_set_emergency(bool on)
{
    if (state_mutex == NULL) return;
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    emergency = on;
    xSemaphoreGive(state_mutex);
}

/* ------------------------------------------------------------------ */
/* 4. SMALL HELPERS                                                    */
/* ------------------------------------------------------------------ */

static user_t *find_user(const char *uid)
{
    for (int i = 0; i < NUM_USERS; i++) {
        if (strcmp(users[i].uid, uid) == 0) {
            return &users[i];
        }
    }
    return NULL;
}

/* bit i set = users[i] blocked (call with state_mutex held) */
static uint32_t blocked_mask_locked(void)
{
    uint32_t mask = 0;
    for (int i = 0; i < NUM_USERS; i++) {
        if (users[i].blocked) mask |= 1u << i;
    }
    return mask;
}

esp_err_t entrance_set_blocked(const char *uid, bool blocked)
{
    if (uid == NULL || state_mutex == NULL) return ESP_ERR_INVALID_STATE;
    char up[UID_LEN];
    int i = 0;
    for (; uid[i] && i < UID_LEN - 1; i++) up[i] = (char)toupper((unsigned char)uid[i]);
    up[i] = '\0';

    xSemaphoreTake(state_mutex, portMAX_DELAY);
    user_t *u = find_user(up);
    if (u != NULL) u->blocked = blocked;
    uint32_t mask = blocked_mask_locked();
    xSemaphoreGive(state_mutex);

    if (u == NULL) return ESP_ERR_NOT_FOUND;
    return storage_kv_set(BLOCKED_KEY, &mask, sizeof(mask));
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
#define BUZZER_HZ  2000                 /* tone of the deny beep */

static esp_err_t gpio_out_init(int pin)
{
    esp_err_t e = gpio_reset_pin(pin);
    if (e == ESP_OK) e = gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    if (e == ESP_OK) e = gpio_set_level(pin, 0);
    return e;
}

/* A hobby servo expects a 50 Hz signal; pulse width 0.5-2.4 ms = 0-180 deg */
static void servo_set_angle(ledc_channel_t ch, int angle)
{
    uint32_t pulse_us = 500 + (uint32_t)angle * 1900 / 180;
    uint32_t duty = pulse_us * ((1u << 14) - 1) / 20000;   /* 20 ms period */
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, ch, duty));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, ch));
}

/* LEDs, two servo channels (timer 0, 50 Hz) and the buzzer (timer 1, 2 kHz).
 * Returns the first error instead of stopping the whole system. */
static esp_err_t hardware_init(void)
{
    esp_err_t e = gpio_out_init(PIN_LED_GRANT);
    if (e == ESP_OK) e = gpio_out_init(PIN_LED_DENY);
    if (e != ESP_OK) return e;

    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_14_BIT,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = 50,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    e = ledc_timer_config(&timer);
    if (e != ESP_OK) return e;

    ledc_timer_config_t buzz_timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num       = LEDC_TIMER_1,
        .freq_hz         = BUZZER_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    e = ledc_timer_config(&buzz_timer);
    if (e != ESP_OK) return e;

    ledc_channel_config_t buzz_ch = {
        .gpio_num   = PIN_BUZZER,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_2,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = LEDC_TIMER_1,
        .duty       = 0,                  /* silent */
        .hpoint     = 0,
    };
    e = ledc_channel_config(&buzz_ch);
    if (e != ESP_OK) return e;

    ledc_channel_config_t entry_ch = {
        .gpio_num   = PIN_ENTRY_SERVO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = 0,
        .hpoint     = 0,
    };
    e = ledc_channel_config(&entry_ch);
    if (e != ESP_OK) return e;

    ledc_channel_config_t exit_ch = entry_ch;
    exit_ch.gpio_num = PIN_EXIT_SERVO;
    exit_ch.channel  = LEDC_CHANNEL_1;
    e = ledc_channel_config(&exit_ch);
    if (e != ESP_OK) return e;

    /* both doors start closed (0 degrees) */
    servo_set_angle(LEDC_CHANNEL_0, 0);
    servo_set_angle(LEDC_CHANNEL_1, 0);
    return ESP_OK;
}

/* buzzer: LEDC plays a 2 kHz square wave (50% duty) for 'ms' milliseconds.
 * vTaskDelay lets other tasks run while it beeps. */
static void beep(int ms)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, 512);   /* 50% of 10 bits */
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2);
    vTaskDelay(pdMS_TO_TICKS(ms));
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2);
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
    char now[16];
    int  count_after;

    /* shared clock and opening hours (schedule component) */
    bool open_now = schedule_is_open();
    clock_str(now, sizeof(now));

    xSemaphoreTake(state_mutex, portMAX_DELAY);          /* ---- lock ---- */

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

    /* CSV record for the "log" command: time,uid,name,point,result,reason */
    snprintf(line, sizeof(line), "%s,%s,%s,%s,%s,%s",
             now, ev->uid, name, point_txt,
             granted ? "GRANTED" : "DENIED", reason);
    log_push_locked(line);

    xSemaphoreGive(state_mutex);                         /* --- unlock --- */

    printf(">> %s %s (%s) -> %s [%s] | inside now: %d\n",
           point_txt, ev->uid, name, granted ? "GRANTED" : "DENIED", reason, count_after);

    /* system log (SD card): one line per decision */
    schedule_log("ACCESS,%s,%s,%s,%s,%s", ev->uid, name, point_txt,
                 granted ? "GRANTED" : "DENIED", reason);

    /* Tell the hub (cocis_system) about this decision. Called outside the lock. */
    if (event_cb != NULL) {
        entrance_event_t pub;
        pub.is_entry = (ev->point == POINT_ENTRY);
        snprintf(pub.uid,  sizeof(pub.uid),  "%s", ev->uid);
        snprintf(pub.name, sizeof(pub.name), "%s", name);
        pub.granted      = granted;
        pub.reason       = reason;          /* always a string literal */
        pub.inside_count = count_after;
        event_cb(&pub);
    }

    actuator_cmd_t cmd = { .door = ev->point, .granted = granted };
    if (xQueueSend(actuator_queue, &cmd, 0) != pdTRUE) {
        printf("actuator busy: door/indicator command dropped\n");
        schedule_log("ACTUATOR,busy,%s %s dropped", point_txt, granted ? "GRANTED" : "DENIED");
    }
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
    printf("\nCommands ([login] = run 'login <pin>' first):\n"
           "  in <UID>            simulate a tag scan at the ENTRY reader\n"
           "  out <UID>           simulate a tag scan at the EXIT reader\n"
           "  inside              who is inside the building now\n"
           "  users               list registered users\n"
           "  log                 last access events (CSV)\n"
           "  status              time, opening hours, emergency mode, count\n"
           "  help                this list\n");
    for (int i = 0; i < ext_cmd_count; i++) {
        printf("  %s\n", ext_cmds[i].help);
    }
    printf("Test UIDs: A1B2C3D4 B2C3D4E5 C3D4E5F6 D4E5F6A7 DEADBEEF(blocked)\n\n");
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
    char now[16];
    int open_min, close_min;
    clock_str(now, sizeof(now));
    schedule_get(&open_min, &close_min);
    bool open_now = schedule_is_open();

    xSemaphoreTake(state_mutex, portMAX_DELAY);
    printf("Time %s | open %02d:%02d - %02d:%02d (%s) | emergency %s | inside %d\n",
           now, open_min / 60, open_min % 60, close_min / 60, close_min % 60,
           open_now ? "OPEN" : "CLOSED",
           emergency ? "ON" : "off", inside_count);
    xSemaphoreGive(state_mutex);
}

static void handle_command(char *line)
{
    char cmd[16] = "", a1[16] = "";
    int n = sscanf(line, "%15s %15s", cmd, a1);
    if (n <= 0) return;

    /* 'rest' = everything after the command word (for registered commands) */
    const char *rest = line;
    while (*rest == ' ') rest++;
    while (*rest && *rest != ' ') rest++;
    while (*rest == ' ') rest++;

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
    } else if (strcmp(cmd, "help") == 0) {
        print_help();
    } else {
        for (int i = 0; i < ext_cmd_count; i++) {
            if (strcmp(cmd, ext_cmds[i].name) == 0) {
                ext_cmds[i].fn(rest);
                return;
            }
        }
        printf("unknown command '%s' (type help)\n", cmd);
    }
}

/* true once the typed line starts with "login " or "setpin ":
 * the PIN typed after it is echoed as '*' */
static bool echo_masked(const char *line, int len)
{
    static const char *const secret_cmds[] = { "login ", "setpin " };
    for (int i = 0; i < 2; i++) {
        int n = (int)strlen(secret_cmds[i]);
        if (len >= n && strncasecmp(line, secret_cmds[i], n) == 0) return true;
    }
    return false;
}

/* The only task that reads the serial port: builds a line, runs it.
 * In a real build, an MFRC522 reader task would fill a scan_event_t
 * and call xQueueSend(scan_queue, ...) instead of the in/out commands. */
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
            bool hide = echo_masked(line, len);
            line[len++] = (char)c;
            uart_write_bytes(UART_NUM_0, hide ? "*" : (const char *)&c, 1);  /* echo it back */
        }
    }
}

/* ------------------------------------------------------------------ */
/* 9. START-UP  (called once by cocis_system/main/main.c)              */
/* ------------------------------------------------------------------ */
esp_err_t entrance_start(void)
{
    if (state_mutex != NULL) {
        return ESP_ERR_INVALID_STATE;                    /* already started */
    }

    state_mutex    = xSemaphoreCreateMutex();
    scan_queue     = xQueueCreate(8, sizeof(scan_event_t));
    actuator_queue = xQueueCreate(8, sizeof(actuator_cmd_t));
    if (state_mutex == NULL || scan_queue == NULL || actuator_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* restore blocked cards saved by block/unblock (keeps the defaults if never saved) */
    uint32_t mask;
    size_t len = sizeof(mask);
    if (storage_kv_get(BLOCKED_KEY, &mask, &len) == ESP_OK && len == sizeof(mask)) {
        for (int i = 0; i < NUM_USERS; i++) {
            users[i].blocked = (mask >> i) & 1u;
        }
    }

    esp_err_t hw = hardware_init();
    if (hw != ESP_OK) {
        printf("entrance hardware init failed: %s\n", esp_err_to_name(hw));
        return hw;
    }

    esp_err_t err = uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }

    /* access and console write to the SD card (FAT), so they get more stack */
    if (xTaskCreate(access_task,   "access",   6144, NULL, 5, NULL) != pdPASS ||
        xTaskCreate(actuator_task, "actuator", 4096, NULL, 4, NULL) != pdPASS ||
        xTaskCreate(console_task,  "console",  6144, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    printf("\n=== CoCIS ENTRANCE (access_control) ready ===\n");
    print_help();
    cmd_status();
    return ESP_OK;
}
