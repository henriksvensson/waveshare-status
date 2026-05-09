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
#include "images.h"
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
#define UI_LEFT_INSET 14
#define USER_NAME_COUNT 5

#define COLOR_BG 0x071013
#define COLOR_TEXT 0xd4dde7
#define COLOR_MUTED 0x94a3b8
#define COLOR_TITLE 0xdff7f6
#define COLOR_HEADER 0x0b2a2d
#define COLOR_CLIENT 0x2dd4bf
#define COLOR_AP 0xf59e0b
#define COLOR_FRESHNESS 0xf59e0b
#define COLOR_STALE 0xff3333

static esp_lcd_panel_io_handle_t lcd_io;
static esp_lcd_panel_handle_t lcd_panel;
static lv_obj_t *startup_container;
static lv_obj_t *status_container;
static lv_obj_t *header_panel;
static lv_obj_t *status_title_label;
static lv_obj_t *status_state_label;
static lv_obj_t *status_wifi_label;
static lv_obj_t *status_ip_label;
static lv_obj_t *status_users_label;
static lv_obj_t *status_user_name_labels[USER_NAME_COUNT];
static lv_obj_t *status_freshness_label;

typedef struct {
    char service[STATUS_TEXT_MAX];
    char wifi[STATUS_TEXT_MAX];
    char ip[STATUS_TEXT_MAX];
    char user_names[USER_NAME_COUNT][STATUS_TEXT_MAX];
    int users;
    bool has_update;
    bool client_mode;
    bool mode_known;
    int64_t last_update_us;
} status_state_t;

static status_state_t status_state = {
    .service = "",
    .wifi = "",
    .ip = "",
    .users = 0,
    .has_update = false,
    .client_mode = false,
    .mode_known = false,
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

static void copy_mode(cJSON *root)
{
    cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "mode");
    if (!cJSON_IsString(mode) || mode->valuestring == NULL) {
        return;
    }

    if (strcmp(mode->valuestring, "client") == 0) {
        status_state.client_mode = true;
        status_state.mode_known = true;
    } else if (strcmp(mode->valuestring, "ap") == 0) {
        status_state.client_mode = false;
        status_state.mode_known = true;
    }
}

static void copy_user_name(cJSON *user, char *dest, size_t dest_size)
{
    if (cJSON_IsString(user) && user->valuestring != NULL) {
        strlcpy(dest, user->valuestring, dest_size);
        return;
    }

    if (cJSON_IsObject(user)) {
        const char *keys[] = {"name", "username", "user"};
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
            cJSON *value = cJSON_GetObjectItemCaseSensitive(user, keys[i]);
            if (cJSON_IsString(value) && value->valuestring != NULL) {
                strlcpy(dest, value->valuestring, dest_size);
                return;
            }
        }
    }
}

static void clear_user_names(void)
{
    for (int i = 0; i < USER_NAME_COUNT; i++) {
        status_state.user_names[i][0] = '\0';
    }
}

static void copy_user_names(cJSON *users, bool update_count)
{
    if (!cJSON_IsArray(users)) {
        return;
    }

    if (update_count) {
        status_state.users = cJSON_GetArraySize(users);
    }
    for (int i = 0; i < USER_NAME_COUNT; i++) {
        cJSON *user = cJSON_GetArrayItem(users, i);
        if (user == NULL) {
            break;
        }
        copy_user_name(user, status_state.user_names[i], sizeof(status_state.user_names[i]));
    }
}

static void show_startup_screen(void)
{
    lv_obj_clear_flag(startup_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(status_container, LV_OBJ_FLAG_HIDDEN);
}

static void show_status_screen(void)
{
    lv_obj_add_flag(startup_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(status_container, LV_OBJ_FLAG_HIDDEN);
}

static void update_mode_label(void)
{
    if (status_state.mode_known) {
        lv_label_set_text(status_state_label, status_state.client_mode ? "Client Mode" : "AP Mode");
        lv_obj_set_style_text_color(status_state_label,
                                    status_state.client_mode ? lv_color_hex(COLOR_CLIENT) : lv_color_hex(COLOR_AP),
                                    0);
        return;
    }

    lv_label_set_text(status_state_label, "[mode unknown]");
    lv_obj_set_style_text_color(status_state_label, lv_color_hex(COLOR_STALE), 0);
}

static void update_users_label(void)
{
    char text[STATUS_TEXT_MAX + 16];
    snprintf(text, sizeof(text), "Users: %d", status_state.users);
    lv_label_set_text(status_users_label, text);
}

static void update_user_name_labels(void)
{
    for (int i = 0; i < USER_NAME_COUNT; i++) {
        lv_label_set_text(status_user_name_labels[i], status_state.user_names[i]);
    }
}

static void update_freshness_label(int64_t age_us)
{
    char text[STATUS_TEXT_MAX + 16];
    const bool stale = age_us < 0 || age_us > STATUS_STALE_US;

    if (!stale) {
        lv_label_set_text(status_freshness_label, "");
        return;
    }

    snprintf(text, sizeof(text), "[!] Stale: %llds ago", age_us / 1000000LL);
    lv_label_set_text(status_freshness_label, text);
}

static void update_status_screen(void)
{
    const int64_t age_us = esp_timer_get_time() - status_state.last_update_us;

    lvgl_port_lock(0);

    if (!status_state.has_update) {
        show_startup_screen();
        lv_refr_now(NULL);
        lvgl_port_unlock();
        return;
    }

    show_status_screen();
    lv_label_set_text(status_title_label, status_state.service[0] != '\0' ? status_state.service : "[service]");
    update_mode_label();
    lv_label_set_text(status_wifi_label, status_state.wifi[0] != '\0' ? status_state.wifi : "[WiFi unknown]");
    lv_label_set_text(status_ip_label, status_state.ip[0] != '\0' ? status_state.ip : "[IP unknown]");
    update_users_label();
    update_user_name_labels();
    update_freshness_label(age_us);
    lv_obj_set_style_text_color(status_freshness_label, lv_color_hex(COLOR_STALE), 0);
    lv_refr_now(NULL);

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
    copy_json_string(root, "wifi", status_state.wifi, sizeof(status_state.wifi));
    copy_json_string(root, "ip", status_state.ip, sizeof(status_state.ip));
    copy_mode(root);
    clear_user_names();

    cJSON *users = cJSON_GetObjectItemCaseSensitive(root, "users");
    if (cJSON_IsNumber(users)) {
        status_state.users = users->valueint;
    } else if (cJSON_IsArray(users)) {
        copy_user_names(users, true);
    }

    cJSON *user_names = cJSON_GetObjectItemCaseSensitive(root, "user_names");
    if (cJSON_IsArray(user_names)) {
        copy_user_names(user_names, !cJSON_IsNumber(users));
    }

    status_state.has_update = true;
    status_state.last_update_us = esp_timer_get_time();
    cJSON_Delete(root);
    ESP_LOGI(TAG, "status update: service=%s mode=%s wifi=%s ip=%s users=%d",
             status_state.service,
             status_state.client_mode ? "client" : "ap",
             status_state.wifi,
             status_state.ip,
             status_state.users);
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

static void create_startup_screen(lv_obj_t *screen)
{
    startup_container = lv_obj_create(screen);
    lv_obj_remove_style_all(startup_container);
    lv_obj_set_size(startup_container, LCD_H_RES, LCD_V_RES);
    lv_obj_set_style_bg_color(startup_container, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(startup_container, LV_OPA_COVER, 0);
    lv_obj_align(startup_container, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *startup_img = lv_img_create(startup_container);
    lv_img_set_src(startup_img, &startup_image);
    lv_obj_align(startup_img, LV_ALIGN_CENTER, 0, 0);
}

static void create_dashboard_screen(lv_obj_t *screen)
{
    status_container = lv_obj_create(screen);
    lv_obj_remove_style_all(status_container);
    lv_obj_set_size(status_container, LCD_H_RES, LCD_V_RES);
    lv_obj_set_style_bg_color(status_container, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(status_container, LV_OPA_COVER, 0);
    lv_obj_align(status_container, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(status_container, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *background_img = lv_img_create(status_container);
    lv_img_set_src(background_img, &background_image);
    lv_obj_align(background_img, LV_ALIGN_CENTER, 0, 0);

    header_panel = lv_obj_create(status_container);
    lv_obj_remove_style_all(header_panel);
    lv_obj_set_size(header_panel, LCD_H_RES, 32);
    lv_obj_set_style_bg_color(header_panel, lv_color_hex(COLOR_HEADER), 0);
    lv_obj_set_style_bg_opa(header_panel, LV_OPA_COVER, 0);
    lv_obj_align(header_panel, LV_ALIGN_TOP_MID, 0, 0);

    status_title_label = lv_label_create(status_container);
    lv_obj_set_style_text_color(status_title_label, lv_color_hex(COLOR_TITLE), 0);
    lv_obj_set_style_text_font(status_title_label, &lv_font_unscii_16, 0);
    lv_obj_set_width(status_title_label, LCD_H_RES - (2 * UI_LEFT_INSET));
    lv_obj_set_style_text_align(status_title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(status_title_label, LV_ALIGN_TOP_MID, 0, 8);

    status_state_label = lv_label_create(status_container);
    lv_obj_set_style_text_font(status_state_label, &lv_font_unscii_16, 0);
    lv_obj_align(status_state_label, LV_ALIGN_TOP_LEFT, UI_LEFT_INSET, 46);

    status_wifi_label = lv_label_create(status_container);
    lv_obj_set_style_text_color(status_wifi_label, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_font(status_wifi_label, &lv_font_unscii_16, 0);
    lv_obj_set_width(status_wifi_label, LCD_H_RES - (2 * UI_LEFT_INSET));
    lv_label_set_long_mode(status_wifi_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(status_wifi_label, LV_ALIGN_TOP_LEFT, UI_LEFT_INSET, 74);

    status_ip_label = lv_label_create(status_container);
    lv_obj_set_style_text_color(status_ip_label, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_font(status_ip_label, &lv_font_unscii_16, 0);
    lv_obj_set_width(status_ip_label, LCD_H_RES - (2 * UI_LEFT_INSET));
    lv_label_set_long_mode(status_ip_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(status_ip_label, LV_ALIGN_TOP_LEFT, UI_LEFT_INSET, 102);

    status_users_label = lv_label_create(status_container);
    lv_obj_set_style_text_color(status_users_label, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_font(status_users_label, &lv_font_unscii_16, 0);
    lv_obj_align(status_users_label, LV_ALIGN_TOP_LEFT, UI_LEFT_INSET, 134);

    for (int i = 0; i < USER_NAME_COUNT; i++) {
        status_user_name_labels[i] = lv_label_create(status_container);
        lv_obj_set_style_text_color(status_user_name_labels[i], lv_color_hex(COLOR_TEXT), 0);
        lv_obj_set_style_text_font(status_user_name_labels[i], &lv_font_unscii_8, 0);
        lv_obj_align(status_user_name_labels[i], LV_ALIGN_TOP_LEFT, UI_LEFT_INSET, 166 + (i * 14));
    }

    status_freshness_label = lv_label_create(status_container);
    lv_obj_set_style_text_font(status_freshness_label, &lv_font_unscii_8, 0);
    lv_obj_set_width(status_freshness_label, LCD_H_RES - (2 * UI_LEFT_INSET));
    lv_obj_set_style_text_align(status_freshness_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(status_freshness_label, LV_ALIGN_BOTTOM_MID, 0, -14);
}

static void create_status_screen(void)
{
    lvgl_port_lock(0);

    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    create_startup_screen(screen);
    create_dashboard_screen(screen);

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
