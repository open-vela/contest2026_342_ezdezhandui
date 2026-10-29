idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver sdmmc esp_driver_sdmmc esp_driver_gpio)
