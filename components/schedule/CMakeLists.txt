idf_component_register(SRCS "schedule.c"
                       INCLUDE_DIRS "include"
                       REQUIRES storage
                       PRIV_REQUIRES esp_timer)
