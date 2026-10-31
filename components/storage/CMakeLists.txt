idf_component_register(SRCS "storage.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES nvs_flash fatfs sdmmc esp_driver_spi esp_driver_sdspi)
