#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"

static const char *TAG = "waveshare-status";

#define LCD_H_RES 240
#define LCD_V_RES 280
#define LCD_SPI_HOST SPI3_HOST
#define LCD_PIXEL_CLOCK_HZ (40 * 1000 * 1000)

#define LCD_PIN_DC GPIO_NUM_4
#define LCD_PIN_CS GPIO_NUM_5
#define LCD_PIN_SCLK GPIO_NUM_6
#define LCD_PIN_MOSI GPIO_NUM_7
#define LCD_PIN_RST GPIO_NUM_8
#define LCD_PIN_BL GPIO_NUM_15

static esp_lcd_panel_handle_t panel;

static uint16_t rgb565(uint8_t red, uint8_t green, uint8_t blue)
{
    return ((red & 0xf8) << 8) | ((green & 0xfc) << 3) | (blue >> 3);
}

static const uint8_t *glyph_for(char ch)
{
    static const uint8_t glyph_space[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    static const uint8_t glyph_a[7] = {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11};
    static const uint8_t glyph_b[7] = {0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e};
    static const uint8_t glyph_d[7] = {0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e};
    static const uint8_t glyph_e[7] = {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f};
    static const uint8_t glyph_f[7] = {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10};
    static const uint8_t glyph_g[7] = {0x0f, 0x10, 0x10, 0x17, 0x11, 0x11, 0x0f};
    static const uint8_t glyph_h[7] = {0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11};
    static const uint8_t glyph_i[7] = {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1f};
    static const uint8_t glyph_l[7] = {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f};
    static const uint8_t glyph_n[7] = {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11};
    static const uint8_t glyph_o[7] = {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e};
    static const uint8_t glyph_r[7] = {0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11};
    static const uint8_t glyph_s[7] = {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e};
    static const uint8_t glyph_t[7] = {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
    static const uint8_t glyph_u[7] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e};
    static const uint8_t glyph_v[7] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04};
    static const uint8_t glyph_w[7] = {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a};

    switch (ch) {
    case 'A': return glyph_a;
    case 'B': return glyph_b;
    case 'D': return glyph_d;
    case 'E': return glyph_e;
    case 'F': return glyph_f;
    case 'G': return glyph_g;
    case 'H': return glyph_h;
    case 'I': return glyph_i;
    case 'L': return glyph_l;
    case 'N': return glyph_n;
    case 'O': return glyph_o;
    case 'R': return glyph_r;
    case 'S': return glyph_s;
    case 'T': return glyph_t;
    case 'U': return glyph_u;
    case 'V': return glyph_v;
    case 'W': return glyph_w;
    case ' ': return glyph_space;
    default: return glyph_space;
    }
}

static void fill_rect(uint16_t *buffer, int x, int y, int width, int height, uint16_t color)
{
    for (int row = y; row < y + height; row++) {
        for (int col = x; col < x + width; col++) {
            buffer[row * LCD_H_RES + col] = color;
        }
    }
}

static void draw_char(uint16_t *buffer, int x, int y, char ch, int scale, uint16_t color)
{
    const uint8_t *glyph = glyph_for(ch);

    for (int row = 0; row < 7; row++) {
        for (int col = 0; col < 5; col++) {
            if ((glyph[row] & (1 << (4 - col))) == 0) {
                continue;
            }

            fill_rect(buffer, x + col * scale, y + row * scale, scale, scale, color);
        }
    }
}

static void draw_text(uint16_t *buffer, int x, int y, const char *text, int scale, uint16_t color)
{
    for (int i = 0; text[i] != '\0'; i++) {
        draw_char(buffer, x + i * 6 * scale, y, text[i], scale, color);
    }
}

static void draw_poc_screen(void)
{
    static uint16_t *buffer;

    if (!buffer) {
        buffer = heap_caps_malloc(LCD_H_RES * LCD_V_RES * sizeof(uint16_t), MALLOC_CAP_DMA);
    }

    if (!buffer) {
        ESP_LOGE(TAG, "failed to allocate LCD framebuffer");
        return;
    }

    const uint16_t background = rgb565(0, 0, 0);
    const uint16_t foreground = rgb565(255, 255, 255);
    const uint16_t muted = rgb565(96, 96, 96);

    for (int i = 0; i < LCD_H_RES * LCD_V_RES; i++) {
        buffer[i] = background;
    }

    fill_rect(buffer, 0, 0, LCD_H_RES, 8, foreground);
    fill_rect(buffer, 20, 82, 200, 3, muted);
    fill_rect(buffer, 20, 222, 200, 3, muted);

    draw_text(buffer, 42, 26, "STATUS", 3, foreground);
    draw_text(buffer, 60, 108, "HELLO", 4, foreground);
    draw_text(buffer, 84, 160, "USB", 4, foreground);
    draw_text(buffer, 72, 236, "HOST", 4, foreground);

    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_H_RES, LCD_V_RES, buffer));
}

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
        .max_transfer_sz = LCD_H_RES * LCD_V_RES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io_handle;
    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = LCD_PIN_DC,
        .cs_gpio_num = LCD_PIN_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 1,
    };
    ESP_LOGI(TAG, "creating LCD SPI IO");
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_config, &io_handle));

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_PIN_RST,
        .color_space = ESP_LCD_COLOR_SPACE_RGB,
        .bits_per_pixel = 16,
    };
    ESP_LOGI(TAG, "creating ST7789 panel");
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel));
    ESP_LOGI(TAG, "resetting ST7789 panel");
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_LOGI(TAG, "initializing ST7789 panel");
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_LOGI(TAG, "configuring ST7789 panel");
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, true, true));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel, 0, 20));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
    ESP_ERROR_CHECK(gpio_set_level(LCD_PIN_BL, 1));
}

void app_main(void)
{
    ESP_LOGI(TAG, "Waveshare status firmware started");
    ESP_LOGI(TAG, "initializing LCD");
    init_lcd();
    draw_poc_screen();
    ESP_LOGI(TAG, "LCD PoC screen drawn");

    while (true) {
        ESP_LOGI(TAG, "heartbeat");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
