idf_component_register(SRCS "main.c" "images.c"
                         "fonts/iosevka_regular_16.c"
                         "fonts/iosevka_regular_20.c"
                         "fonts/iosevka_bold_24.c"
                    INCLUDE_DIRS "."
                    EMBED_FILES "../assets/startup_rgb565.bin" "../assets/background_rgb565.bin"
                    REQUIRES esp_lcd esp_driver_gpio esp_driver_spi esp_lvgl_port json lvgl)
