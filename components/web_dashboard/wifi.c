/* wifi.c - connects the ESP32 to the Wokwi Wi-Fi network (station mode).
 *
 * Event driven, never blocks boot:
 *   wifi_net_start()  -> starts the driver and returns at once
 *   STA_START         -> esp_wifi_connect()
 *   GOT_IP            -> print "[WIFI] connected, IP x.x.x.x" and log it
 *   STA_DISCONNECTED  -> try again (Wi-Fi keeps retrying in the background)
 * The handlers run in the ESP-IDF event task. */
#include <stdio.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "schedule.h"
#include "web_dashboard.h"

#define WIFI_SSID     "Wokwi-GUEST"   /* Wokwi's simulated access point: open */
#define WIFI_CHANNEL  6               /* Wokwi-GUEST is on channel 6 (faster) */

/* Written only by the event task; read by others. A 32-bit value / flag is
 * read in one instruction on the ESP32, so no mutex is needed here. */
static volatile bool     s_connected = false;
static volatile uint32_t s_ip = 0;          /* IPv4 address, network order */

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        printf("[WIFI] connecting to %s ...\n", WIFI_SSID);
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_connected) {                  /* log only the first loss */
            printf("[WIFI] disconnected, retrying\n");
            schedule_log("WIFI,disconnected");
        }
        s_connected = false;
        esp_wifi_connect();                 /* retry forever */
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        s_ip = ev->ip_info.ip.addr;
        s_connected = true;
        printf("[WIFI] connected, IP " IPSTR "\n", IP2STR(&ev->ip_info.ip));
        printf("[WEB] open http://localhost:8180 (Wokwi port forward)\n");
        schedule_log("WIFI,connected," IPSTR, IP2STR(&ev->ip_info.ip));
    }
}

esp_err_t wifi_net_start(void)
{
    /* NVS (needed by Wi-Fi) is already initialised by storage_init(). */
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;  /* may exist already */
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL));

    wifi_config_t wc = { 0 };
    strcpy((char *)wc.sta.ssid, WIFI_SSID);
    wc.sta.channel = WIFI_CHANNEL;
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;  /* no password */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());           /* returns at once */
    return ESP_OK;
}

bool wifi_net_connected(void)
{
    return s_connected;
}

const char *wifi_net_ip(char *buf, int len)
{
    if (!s_connected) {
        snprintf(buf, len, "-");
    } else {
        esp_ip4_addr_t ip = { .addr = s_ip };
        snprintf(buf, len, IPSTR, IP2STR(&ip));
    }
    return buf;
}
