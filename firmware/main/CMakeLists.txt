idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_lcd esp_driver_gpio esp_driver_spi esp_lvgl_port lvgl)
