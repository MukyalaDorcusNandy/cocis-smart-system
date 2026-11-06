idf_component_register(SRCS "rfid.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_gpio esp_rom access_control)
