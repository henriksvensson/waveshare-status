idf_component_register(SRCS "main.c" "startup_image.c"
                    INCLUDE_DIRS "."
                    EMBED_FILES "../assets/startup_rgb565.bin"
                    REQUIRES esp_lcd esp_driver_gpio esp_driver_spi esp_lvgl_port json lvgl)
