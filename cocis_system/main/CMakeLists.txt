idf_component_register(SRCS "main.c" "hub.c"
                       INCLUDE_DIRS "."
                       REQUIRES room_control access_control storage schedule auth management esp_timer)
