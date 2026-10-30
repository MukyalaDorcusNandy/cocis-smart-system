idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES room_control access_control driver esp_driver_uart)