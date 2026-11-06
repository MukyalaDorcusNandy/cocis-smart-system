/* web_dashboard.c - HTTP server for the security dashboard.
 *
 * Tasks that touch this file's data:
 *   - entrance task : on_entrance_event() adds an alert to the ring
 *   - httpd task    : the API handlers read the ring and the web session
 *   - console task  : "wifi" command (reads Wi-Fi state only)
 * The alert ring and the session token are shared between the entrance task
 * and the httpd task, so ONE mutex (s_lock) protects both. Every access is a
 * short copy inside the lock: nobody waits on the network or the SD card
 * while holding it, so the entrance task is never slowed down. */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_http_server.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "access_control.h"
#include "schedule.h"
#include "auth.h"
#include "web_dashboard.h"

#define ALERT_MAX         30          /* alerts kept in RAM (ring buffer)    */
#define TOKEN_LEN         16          /* hex characters                      */
#define SESSION_IDLE_S    600         /* web session ends after 10 min idle  */
#define MAX_USERS         32
#define MAX_ROOMS         8
#define JSON_BUF          12288       /* /api/state answer (heap, per call)  */

/* The page, embedded at build time from dashboard.html (see CMakeLists.txt) */
extern const char dashboard_html_start[] asm("_binary_dashboard_html_start");
extern const char dashboard_html_end[]   asm("_binary_dashboard_html_end");

/* One alert = one scan decision from the entrance */
typedef struct {
    uint32_t        id;               /* 1, 2, 3 ... so the page sees new ones */
    char            time[12];         /* "HH:MM:SS"                           */
    char            uid[ENTRANCE_UID_LEN];
    char            name[ENTRANCE_NAME_LEN];
    entrance_role_t role;
    bool            known, is_entry, granted;
    const char     *reason;           /* string literal from access_control   */
} alert_t;

/* ---- shared data, protected by s_lock ---- */
static alert_t  s_alerts[ALERT_MAX];
static int      s_alert_head = 0;     /* next slot to write  */
static int      s_alert_count = 0;
static uint32_t s_alert_id = 0;
static char     s_token[TOKEN_LEN + 1] = "";   /* "" = nobody signed in */
static int64_t  s_token_used_us = 0;           /* last request with the token */
static SemaphoreHandle_t s_lock;

static web_room_fn_t s_room_fn = NULL;
static httpd_handle_t s_server = NULL;

/* ================= alerts (entrance task) ================= */

/* Called by the entrance task after every scan. Must not block: it only
 * copies the event into the ring (oldest alert is overwritten). */
static void on_entrance_event(const entrance_event_t *ev)
{
    if (s_lock == NULL) return;
    alert_t a = { 0 };
    clock_str(a.time, sizeof(a.time));
    strlcpy(a.uid, ev->uid, sizeof(a.uid));
    strlcpy(a.name, ev->name, sizeof(a.name));
    a.role = ev->role;
    a.known = ev->known;
    a.is_entry = ev->is_entry;
    a.granted = ev->granted;
    a.reason = ev->reason ? ev->reason : "";

    xSemaphoreTake(s_lock, portMAX_DELAY);
    a.id = ++s_alert_id;
    s_alerts[s_alert_head] = a;
    s_alert_head = (s_alert_head + 1) % ALERT_MAX;
    if (s_alert_count < ALERT_MAX) s_alert_count++;
    xSemaphoreGive(s_lock);
}

/* ================= session token ================= */

/* Makes a new random token (16 hex chars). Only one web session at a time:
 * a new sign-in replaces the old token. */
static void new_token(char *out)
{
    uint32_t r[2] = { esp_random(), esp_random() };
    char t[TOKEN_LEN + 1];
    snprintf(t, sizeof(t), "%08lx%08lx", (unsigned long)r[0], (unsigned long)r[1]);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strcpy(s_token, t);
    s_token_used_us = esp_timer_get_time();
    xSemaphoreGive(s_lock);
    strcpy(out, t);
}

/* true if the request carries the current token and it is not idle too long.
 * A valid request restarts the 10-minute idle timer. */
static bool token_ok(httpd_req_t *req)
{
    char got[TOKEN_LEN + 1] = "";
    if (httpd_req_get_hdr_value_str(req, "X-Token", got, sizeof(got)) != ESP_OK) return false;

    bool ok = false, expired = false;
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_token[0] != '\0' && strcmp(got, s_token) == 0) {
        if (now - s_token_used_us > (int64_t)SESSION_IDLE_S * 1000000) {
            s_token[0] = '\0';             /* idle too long: session over */
            expired = true;
        } else {
            s_token_used_us = now;
            ok = true;
        }
    }
    xSemaphoreGive(s_lock);
    if (expired) schedule_log("WEB,session timeout");
    return ok;
}

/* ================= small helpers ================= */

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *json)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t send_401(httpd_req_t *req)
{
    return send_json(req, "401 Unauthorized", "{\"ok\":false,\"error\":\"sign in\"}");
}

/* Reads a small form body ("a=1&b=2") into buf. */
static bool read_body(httpd_req_t *req, char *buf, size_t len)
{
    if (req->content_len >= len) return false;
    int got = 0;
    while (got < (int)req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r <= 0) return false;
        got += r;
    }
    buf[got] = '\0';
    return true;
}

/* Gets one form value; '%3A' (URL-encoded ':') is turned back into ':'. */
static bool form_get(const char *body, const char *key, char *val, size_t len)
{
    if (httpd_query_key_value(body, key, val, len) != ESP_OK) return false;
    char *p;
    while ((p = strstr(val, "%3A")) != NULL || (p = strstr(val, "%3a")) != NULL) {
        *p = ':';
        memmove(p + 1, p + 3, strlen(p + 3) + 1);
    }
    return true;
}

/* Copies text that goes inside a JSON string; quotes, backslashes and
 * control characters become '?' (UIDs typed on the console could hold them). */
static const char *js(const char *in, char *out, size_t len)
{
    size_t i = 0;
    for (; in[i] && i < len - 1; i++) {
        char c = in[i];
        out[i] = (c == '"' || c == '\\' || (unsigned char)c < 0x20) ? '?' : c;
    }
    out[i] = '\0';
    return out;
}

/* Appends printf-style text to the JSON buffer, never past its end. */
typedef struct { char *buf; size_t len, used; } jbuf_t;

static void add(jbuf_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void add(jbuf_t *b, const char *fmt, ...)
{
    if (b->used >= b->len - 1) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b->buf + b->used, b->len - b->used, fmt, ap);
    va_end(ap);
    if (n > 0) b->used += n;
    if (b->used > b->len - 1) b->used = b->len - 1;
}

/* ================= handlers ================= */

static esp_err_t h_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, dashboard_html_start,
                           dashboard_html_end - dashboard_html_start);
}

/* POST /api/login  pin=1234 */
static esp_err_t h_login(httpd_req_t *req)
{
    char body[64], pin[16] = "", out[96];
    if (!read_body(req, body, sizeof(body)) || !form_get(body, "pin", pin, sizeof(pin))) {
        return send_json(req, "400 Bad Request", "{\"ok\":false,\"error\":\"missing pin\"}");
    }
    int locked = auth_lockout_remaining_s();
    if (locked > 0) {                       /* say how long, do not count a try */
        snprintf(out, sizeof(out), "{\"ok\":false,\"error\":\"Locked: try again in %d s\"}", locked);
        return send_json(req, "403 Forbidden", out);
    }
    if (!auth_check_pin(pin)) {             /* shares the console's lockout counter */
        locked = auth_lockout_remaining_s();
        if (locked > 0) {
            snprintf(out, sizeof(out), "{\"ok\":false,\"error\":\"Too many wrong PINs: locked for %d s\"}", locked);
        } else {
            snprintf(out, sizeof(out), "{\"ok\":false,\"error\":\"Wrong PIN\"}");
        }
        return send_json(req, "403 Forbidden", out);
    }
    char tok[TOKEN_LEN + 1];
    new_token(tok);
    snprintf(out, sizeof(out), "{\"ok\":true,\"token\":\"%s\"}", tok);
    return send_json(req, "200 OK", out);
}

/* POST /api/logout */
static esp_err_t h_logout(httpd_req_t *req)
{
    if (token_ok(req)) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_token[0] = '\0';
        xSemaphoreGive(s_lock);
        schedule_log("WEB,logout");
    }
    return send_json(req, "200 OK", "{\"ok\":true}");
}

/* GET /api/state - everything the page shows, in one JSON object */
static esp_err_t h_state(httpd_req_t *req)
{
    if (!token_ok(req)) return send_401(req);

    /* Big arrays live on the heap, not on the small httpd task stack */
    entrance_user_t *users = malloc(sizeof(entrance_user_t) * MAX_USERS);
    alert_t *alerts = malloc(sizeof(alert_t) * ALERT_MAX);
    jbuf_t b = { .buf = malloc(JSON_BUF), .len = JSON_BUF, .used = 0 };
    if (users == NULL || alerts == NULL || b.buf == NULL) {
        free(users); free(alerts); free(b.buf);
        return send_json(req, "500 Internal Server Error", "{\"ok\":false}");
    }

    /* 1. copy the alerts newest first, inside the lock (short) */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int na = s_alert_count;
    for (int i = 0; i < na; i++) {
        alerts[i] = s_alerts[(s_alert_head - 1 - i + ALERT_MAX) % ALERT_MAX];
    }
    xSemaphoreGive(s_lock);

    /* 2. building state (each getter is thread safe itself) */
    int nu = entrance_get_users(users, MAX_USERS);
    int open_min, close_min;
    schedule_get(&open_min, &close_min);
    char now[12], t1[40], t2[40];
    clock_str(now, sizeof(now));

    int per_role[ROLE_COUNT] = { 0 };
    for (int i = 0; i < nu; i++) {
        if (users[i].inside && users[i].role < ROLE_COUNT) per_role[users[i].role]++;
    }

    add(&b, "{\"ok\":true,\"time\":\"%s\",\"open\":\"%02d:%02d\",\"close\":\"%02d:%02d\","
            "\"is_open\":%s,\"emergency\":%s,\"inside_count\":%d,\"wifi_ip\":\"%s\",",
        now, open_min / 60, open_min % 60, close_min / 60, close_min % 60,
        schedule_is_open() ? "true" : "false",
        entrance_get_emergency() ? "true" : "false",
        entrance_inside_count(), wifi_net_ip(t1, sizeof(t1)));

    add(&b, "\"roles\":[");
    for (int r = 0; r < ROLE_COUNT; r++) {
        add(&b, "%s{\"role\":\"%s\",\"color\":\"%s\",\"inside\":%d}", r ? "," : "",
            entrance_role_name(r), entrance_role_color(r), per_role[r]);
    }

    add(&b, "],\"users\":[");
    for (int i = 0; i < nu; i++) {
        add(&b, "%s{\"uid\":\"%s\",\"name\":\"%s\",\"role\":\"%s\",\"color\":\"%s\","
                "\"inside\":%s,\"blocked\":%s,\"since\":\"%s\"}", i ? "," : "",
            js(users[i].uid, t1, sizeof(t1)), js(users[i].name, t2, sizeof(t2)),
            entrance_role_name(users[i].role), entrance_role_color(users[i].role),
            users[i].inside ? "true" : "false", users[i].blocked ? "true" : "false",
            users[i].since);
    }

    add(&b, "],\"rooms\":[");
    for (int r = 1; s_room_fn != NULL && r <= MAX_ROOMS; r++) {
        bool online, occupied, light;
        uint32_t on_s;
        if (!s_room_fn(r, &online, &occupied, &light, &on_s)) break;
        add(&b, "%s{\"room\":%d,\"online\":%s,\"occupied\":%s,\"light\":%s,\"light_on_s\":%lu}",
            r > 1 ? "," : "", r, online ? "true" : "false", occupied ? "true" : "false",
            light ? "true" : "false", (unsigned long)on_s);
    }

    add(&b, "],\"alerts\":[");
    for (int i = 0; i < na; i++) {
        alert_t *a = &alerts[i];
        add(&b, "%s{\"id\":%lu,\"time\":\"%s\",\"uid\":\"%s\",\"name\":\"%s\",\"role\":\"%s\","
                "\"color\":\"%s\",\"known\":%s,\"entry\":%s,\"granted\":%s,\"reason\":\"%s\"}",
            i ? "," : "", (unsigned long)a->id, a->time,
            js(a->uid, t1, sizeof(t1)), js(a->name, t2, sizeof(t2)),
            a->known ? entrance_role_name(a->role) : "Unknown",
            a->known ? entrance_role_color(a->role) : "red",
            a->known ? "true" : "false", a->is_entry ? "true" : "false",
            a->granted ? "true" : "false", a->reason);
    }
    add(&b, "]}");

    esp_err_t e = send_json(req, "200 OK", b.buf);
    free(users); free(alerts); free(b.buf);
    return e;
}

/* POST /api/hours  open=HH:MM&close=HH:MM */
static esp_err_t h_hours(httpd_req_t *req)
{
    if (!token_ok(req)) return send_401(req);
    char body[96], o[16] = "", c[16] = "";
    int oh, om, ch, cm;
    if (!read_body(req, body, sizeof(body)) || !form_get(body, "open", o, sizeof(o)) ||
        !form_get(body, "close", c, sizeof(c)) ||
        sscanf(o, "%d:%d", &oh, &om) != 2 || sscanf(c, "%d:%d", &ch, &cm) != 2 ||
        oh < 0 || oh > 23 || om < 0 || om > 59 || ch < 0 || ch > 23 || cm < 0 || cm > 59 ||
        schedule_set(oh * 60 + om, ch * 60 + cm) != ESP_OK) {
        return send_json(req, "400 Bad Request",
                         "{\"ok\":false,\"error\":\"Use HH:MM, opening before closing\"}");
    }
    schedule_log("WEB,hours %02d:%02d-%02d:%02d", oh, om, ch, cm);
    return send_json(req, "200 OK", "{\"ok\":true}");
}

/* POST /api/emergency  on=1|0 */
static esp_err_t h_emergency(httpd_req_t *req)
{
    if (!token_ok(req)) return send_401(req);
    char body[32], v[4] = "";
    if (!read_body(req, body, sizeof(body)) || !form_get(body, "on", v, sizeof(v))) {
        return send_json(req, "400 Bad Request", "{\"ok\":false,\"error\":\"missing on\"}");
    }
    bool on = (v[0] == '1');
    entrance_set_emergency(on);
    schedule_log("WEB,emergency %s", on ? "ON" : "off");
    return send_json(req, "200 OK", "{\"ok\":true}");
}

/* POST /api/block  uid=XXXX&blocked=1|0 */
static esp_err_t h_block(httpd_req_t *req)
{
    if (!token_ok(req)) return send_401(req);
    char body[96], uid[ENTRANCE_UID_LEN] = "", v[4] = "";
    if (!read_body(req, body, sizeof(body)) || !form_get(body, "uid", uid, sizeof(uid)) ||
        !form_get(body, "blocked", v, sizeof(v))) {
        return send_json(req, "400 Bad Request", "{\"ok\":false,\"error\":\"missing uid\"}");
    }
    bool blocked = (v[0] == '1');
    if (entrance_set_blocked(uid, blocked) != ESP_OK) {
        return send_json(req, "404 Not Found", "{\"ok\":false,\"error\":\"unknown card\"}");
    }
    schedule_log("WEB,%s %s", blocked ? "block" : "unblock", uid);
    return send_json(req, "200 OK", "{\"ok\":true}");
}

/* POST /api/scan  uid=XXXX&entry=1|0 - test scan, same as "in/out <UID>" */
static esp_err_t h_scan(httpd_req_t *req)
{
    if (!token_ok(req)) return send_401(req);
    char body[96], uid[ENTRANCE_UID_LEN] = "", v[4] = "1";
    if (!read_body(req, body, sizeof(body)) || !form_get(body, "uid", uid, sizeof(uid)) ||
        uid[0] == '\0') {
        return send_json(req, "400 Bad Request", "{\"ok\":false,\"error\":\"missing uid\"}");
    }
    form_get(body, "entry", v, sizeof(v));
    bool entry = (v[0] != '0');
    if (entrance_submit_scan(entry, uid) != ESP_OK) {
        return send_json(req, "503 Service Unavailable", "{\"ok\":false,\"error\":\"scan queue busy\"}");
    }
    schedule_log("WEB,test scan %s %s", entry ? "in" : "out", uid);
    return send_json(req, "200 OK", "{\"ok\":true}");
}

/* ================= console command ================= */

static void cmd_wifi(const char *args)
{
    (void)args;
    char ip[20];
    printf("Wi-Fi: %s, IP %s, web server %s\n",
           wifi_net_connected() ? "connected" : "not connected",
           wifi_net_ip(ip, sizeof(ip)), s_server ? "running on port 80" : "stopped");
    printf("Dashboard: open http://localhost:8180 (Wokwi port forward)\n");
}

/* ================= start ================= */

void web_dashboard_set_room_provider(web_room_fn_t fn)
{
    s_room_fn = fn;
}

esp_err_t web_dashboard_register_commands(void)
{
    if (s_lock == NULL) s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;
    entrance_set_event_cb(on_entrance_event);      /* alerts for the feed */
    return entrance_register_command("wifi", "wifi                Wi-Fi status and dashboard address",
                                     cmd_wifi);
}

esp_err_t web_dashboard_start(void)
{
    if (s_lock == NULL) return ESP_ERR_INVALID_STATE;   /* register first */

    esp_err_t e = wifi_net_start();
    if (e != ESP_OK) {
        printf("[WIFI] start failed: %s (system runs without the dashboard)\n", esp_err_to_name(e));
        return ESP_OK;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.max_uri_handlers = 10;
    cfg.stack_size = 6144;
    cfg.lru_purge_enable = true;       /* reuse sockets when the browser keeps many open */
    e = httpd_start(&s_server, &cfg);
    if (e != ESP_OK) {
        printf("[WEB] server start failed: %s\n", esp_err_to_name(e));
        s_server = NULL;
        return ESP_OK;
    }

    const httpd_uri_t uris[] = {
        { .uri = "/",              .method = HTTP_GET,  .handler = h_page },
        { .uri = "/api/state",     .method = HTTP_GET,  .handler = h_state },
        { .uri = "/api/login",     .method = HTTP_POST, .handler = h_login },
        { .uri = "/api/logout",    .method = HTTP_POST, .handler = h_logout },
        { .uri = "/api/hours",     .method = HTTP_POST, .handler = h_hours },
        { .uri = "/api/emergency", .method = HTTP_POST, .handler = h_emergency },
        { .uri = "/api/block",     .method = HTTP_POST, .handler = h_block },
        { .uri = "/api/scan",      .method = HTTP_POST, .handler = h_scan },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }
    printf("[WEB] dashboard on port 80\n");
    schedule_log("WEB,server started");
    return ESP_OK;
}
