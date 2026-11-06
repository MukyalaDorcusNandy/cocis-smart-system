# dashboard.html is a normal readable file; EMBED_TXTFILES puts it into the
# firmware (zero-terminated) as _binary_dashboard_html_start/_end.
idf_component_register(SRCS "web_dashboard.c" "wifi.c"
                       INCLUDE_DIRS "include"
                       EMBED_TXTFILES "dashboard.html"
                       PRIV_REQUIRES esp_wifi esp_netif esp_event esp_http_server esp_timer
                                     access_control schedule auth)
