idf_component_register(SRCS "lcd_display.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_i2c access_control schedule)
