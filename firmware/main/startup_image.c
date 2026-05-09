#include "startup_image.h"

#define STARTUP_IMAGE_WIDTH 240
#define STARTUP_IMAGE_HEIGHT 280
#define STARTUP_IMAGE_BYTES_PER_PIXEL 2

extern const uint8_t startup_rgb565_bin_start[] asm("_binary_startup_rgb565_bin_start");
extern const uint8_t background_rgb565_bin_start[] asm("_binary_background_rgb565_bin_start");

const lv_img_dsc_t startup_image = {
    .header = {
        .cf = LV_IMG_CF_TRUE_COLOR,
        .always_zero = 0,
        .reserved = 0,
        .w = STARTUP_IMAGE_WIDTH,
        .h = STARTUP_IMAGE_HEIGHT,
    },
    .data_size = STARTUP_IMAGE_WIDTH * STARTUP_IMAGE_HEIGHT * STARTUP_IMAGE_BYTES_PER_PIXEL,
    .data = startup_rgb565_bin_start,
};

const lv_img_dsc_t background_image = {
    .header = {
        .cf = LV_IMG_CF_TRUE_COLOR,
        .always_zero = 0,
        .reserved = 0,
        .w = STARTUP_IMAGE_WIDTH,
        .h = STARTUP_IMAGE_HEIGHT,
    },
    .data_size = STARTUP_IMAGE_WIDTH * STARTUP_IMAGE_HEIGHT * STARTUP_IMAGE_BYTES_PER_PIXEL,
    .data = background_rgb565_bin_start,
};
