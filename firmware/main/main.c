#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "cJSON.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "lvgl.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "waveshare-status";

#define LCD_H_RES 240
#define LCD_V_RES 280
#define LCD_SPI_HOST SPI3_HOST
#define LCD_PIXEL_CLOCK_HZ (40 * 1000 * 1000)
#define LCD_DRAW_BUFFER_LINES 50

#define LCD_PIN_DC GPIO_NUM_4
#define LCD_PIN_CS GPIO_NUM_5
#define LCD_PIN_SCLK GPIO_NUM_6
#define LCD_PIN_MOSI GPIO_NUM_7
#define LCD_PIN_RST GPIO_NUM_8
#define LCD_PIN_BL GPIO_NUM_15

#define SERIAL_LINE_MAX 256
#define STATUS_TEXT_MAX 64
#define STATUS_STALE_US (30 * 1000 * 1000LL)

static esp_lcd_panel_io_handle_t lcd_io;
static esp_lcd_panel_handle_t lcd_panel;
static lv_obj_t *status_title_label;
static lv_obj_t *status_state_label;
static lv_obj_t *status_host_label;
static lv_obj_t *status_ip_label;
static lv_obj_t *status_users_label;
static lv_obj_t *status_freshness_label;

typedef struct {
    char service[STATUS_TEXT_MAX];
    char host[STATUS_TEXT_MAX];
    char ip[STATUS_TEXT_MAX];
    int users;
    int max_users;
    bool online;
    int64_t last_update_us;
} status_state_t;

static status_state_t status_state = {
    .service = "MURMUR",
    .host = "waiting",
    .ip = "--",
    .users = 0,
    .max_users = 32,
    .online = false,
};

static void init_lcd(void)
{
    gpio_config_t backlight_config = {
        .pin_bit_mask = 1ULL << LCD_PIN_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&backlight_config));
    ESP_ERROR_CHECK(gpio_set_level(LCD_PIN_BL, 0));

    const spi_bus_config_t bus_config = {
        .sclk_io_num = LCD_PIN_SCLK,
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_H_RES * LCD_DRAW_BUFFER_LINES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO));

    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = LCD_PIN_DC,
        .cs_gpio_num = LCD_PIN_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_config, &lcd_io));

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_PIN_RST,
        .color_space = ESP_LCD_COLOR_SPACE_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(lcd_io, &panel_config, &lcd_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(lcd_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(lcd_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(lcd_panel, true, true));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(lcd_panel, 0, 20));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(lcd_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(lcd_panel, true));
    ESP_ERROR_CHECK(gpio_set_level(LCD_PIN_BL, 1));
}

static void init_lvgl(void)
{
    const lvgl_port_cfg_t lvgl_config = {
        .task_priority = 4,
        .task_stack = 4096,
        .task_affinity = -1,
        .task_max_sleep_ms = 500,
        .timer_period_ms = 5,
    };
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_config));

    const lvgl_port_display_cfg_t display_config = {
        .io_handle = lcd_io,
        .panel_handle = lcd_panel,
        .buffer_size = LCD_H_RES * LCD_DRAW_BUFFER_LINES * sizeof(uint16_t),
        .double_buffer = true,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = true,
        },
    };
    lvgl_port_add_disp(&display_config);
}

static void copy_json_string(cJSON *root, const char *name, char *dest, size_t dest_size)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(root, name);
    if (cJSON_IsString(value) && value->valuestring != NULL) {
        strlcpy(dest, value->valuestring, dest_size);
    }
}

static void update_status_screen(void)
{
    char text[STATUS_TEXT_MAX + 16];
    const int64_t age_us = status_state.last_update_us == 0 ? -1 : esp_timer_get_time() - status_state.last_update_us;
    const bool stale = age_us < 0 || age_us > STATUS_STALE_US;

    lvgl_port_lock(0);

    lv_label_set_text(status_title_label, status_state.service);
    lv_label_set_text(status_state_label, status_state.online && !stale ? "ONLINE" : "OFFLINE");
    lv_obj_set_style_text_color(status_state_label,
                                status_state.online && !stale ? lv_color_hex(0x28d17c) : lv_color_hex(0xf97316),
                                0);

    snprintf(text, sizeof(text), "Host: %s", status_state.host);
    lv_label_set_text(status_host_label, text);

    snprintf(text, sizeof(text), "IP: %s", status_state.ip);
    lv_label_set_text(status_ip_label, text);

    snprintf(text, sizeof(text), "Users: %d / %d", status_state.users, status_state.max_users);
    lv_label_set_text(status_users_label, text);

    if (age_us < 0) {
        lv_label_set_text(status_freshness_label, "Last update: never");
    } else if (stale) {
        snprintf(text, sizeof(text), "Stale: %llds ago", age_us / 1000000LL);
        lv_label_set_text(status_freshness_label, text);
    } else {
        snprintf(text, sizeof(text), "Last update: %llds ago", age_us / 1000000LL);
        lv_label_set_text(status_freshness_label, text);
    }
    lv_obj_set_style_text_color(status_freshness_label, stale ? lv_color_hex(0xfbbf24) : lv_color_hex(0x64748b), 0);

    lvgl_port_unlock();
}

static void apply_status_json(const char *line)
{
    cJSON *root = cJSON_Parse(line);
    if (root == NULL) {
        ESP_LOGW(TAG, "ignoring invalid JSON status line");
        return;
    }

    copy_json_string(root, "service", status_state.service, sizeof(status_state.service));
    copy_json_string(root, "host", status_state.host, sizeof(status_state.host));
    copy_json_string(root, "ip", status_state.ip, sizeof(status_state.ip));

    cJSON *online = cJSON_GetObjectItemCaseSensitive(root, "online");
    if (cJSON_IsBool(online)) {
        status_state.online = cJSON_IsTrue(online);
    }

    cJSON *users = cJSON_GetObjectItemCaseSensitive(root, "users");
    if (cJSON_IsNumber(users)) {
        status_state.users = users->valueint;
    }

    cJSON *max_users = cJSON_GetObjectItemCaseSensitive(root, "max_users");
    if (cJSON_IsNumber(max_users)) {
        status_state.max_users = max_users->valueint;
    }

    status_state.last_update_us = esp_timer_get_time();
    cJSON_Delete(root);
    ESP_LOGI(TAG, "status update: service=%s online=%d host=%s ip=%s users=%d/%d",
             status_state.service,
             status_state.online,
             status_state.host,
             status_state.ip,
             status_state.users,
             status_state.max_users);
    update_status_screen();
}

static void serial_status_task(void *arg)
{
    char line[SERIAL_LINE_MAX];
    size_t len = 0;

    while (true) {
        int ch = getchar();
        if (ch == EOF) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (ch == '\r') {
            continue;
        }

        if (ch == '\n') {
            line[len] = '\0';
            if (len > 0) {
                apply_status_json(line);
            }
            len = 0;
            continue;
        }

        if (len < sizeof(line) - 1) {
            line[len++] = (char)ch;
        } else {
            len = 0;
            ESP_LOGW(TAG, "dropping oversized status line");
        }
    }
}

static void freshness_task(void *arg)
{
    while (true) {
        update_status_screen();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void create_status_screen(void)
{
    lvgl_port_lock(0);

    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x05070d), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    lv_obj_t *accent = lv_obj_create(screen);
    lv_obj_remove_style_all(accent);
    lv_obj_set_size(accent, 6, 280);
    lv_obj_set_style_bg_color(accent, lv_color_hex(0x38bdf8), 0);
    lv_obj_set_style_bg_opa(accent, LV_OPA_COVER, 0);
    lv_obj_align(accent, LV_ALIGN_LEFT_MID, 0, 0);

    status_title_label = lv_label_create(screen);
    lv_obj_set_style_text_color(status_title_label, lv_color_hex(0x94a3b8), 0);
    lv_obj_set_style_text_font(status_title_label, &lv_font_montserrat_14, 0);
    lv_obj_align(status_title_label, LV_ALIGN_TOP_LEFT, 20, 18);

    status_state_label = lv_label_create(screen);
    lv_obj_set_style_text_font(status_state_label, &lv_font_montserrat_28, 0);
    lv_obj_align(status_state_label, LV_ALIGN_TOP_LEFT, 20, 42);

    status_host_label = lv_label_create(screen);
    lv_obj_set_style_text_color(status_host_label, lv_color_hex(0xe2e8f0), 0);
    lv_obj_set_style_text_font(status_host_label, &lv_font_montserrat_16, 0);
    lv_obj_align(status_host_label, LV_ALIGN_TOP_LEFT, 20, 88);

    status_ip_label = lv_label_create(screen);
    lv_obj_set_style_text_color(status_ip_label, lv_color_hex(0xcbd5e1), 0);
    lv_obj_set_style_text_font(status_ip_label, &lv_font_montserrat_16, 0);
    lv_obj_align(status_ip_label, LV_ALIGN_TOP_LEFT, 20, 118);

    status_users_label = lv_label_create(screen);
    lv_obj_set_style_text_color(status_users_label, lv_color_hex(0xcbd5e1), 0);
    lv_obj_set_style_text_font(status_users_label, &lv_font_montserrat_16, 0);
    lv_obj_align(status_users_label, LV_ALIGN_TOP_LEFT, 20, 148);

    status_freshness_label = lv_label_create(screen);
    lv_obj_set_style_text_font(status_freshness_label, &lv_font_montserrat_14, 0);
    lv_obj_align(status_freshness_label, LV_ALIGN_TOP_LEFT, 20, 188);

    lv_obj_t *footer = lv_label_create(screen);
    lv_label_set_text(footer, "USB serial status display");
    lv_obj_set_style_text_color(footer, lv_color_hex(0x64748b), 0);
    lv_obj_set_style_text_font(footer, &lv_font_montserrat_14, 0);
    lv_obj_align(footer, LV_ALIGN_BOTTOM_LEFT, 20, -20);

    lvgl_port_unlock();
    update_status_screen();
}

void app_main(void)
{
    ESP_LOGI(TAG, "Waveshare status firmware started");
    ESP_LOGI(TAG, "initializing LCD and LVGL");
    init_lcd();
    init_lvgl();
    create_status_screen();
    xTaskCreate(serial_status_task, "serial_status", 4096, NULL, 5, NULL);
    xTaskCreate(freshness_task, "status_freshness", 4096, NULL, 3, NULL);
    ESP_LOGI(TAG, "LVGL status screen created");

    while (true) {
        ESP_LOGI(TAG, "heartbeat");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
