idf_component_register(SRCS "access_control.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_gpio esp_driver_ledc esp_driver_uart esp_timer
                                     schedule storage)
