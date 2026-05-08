#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "waveshare-status";

void app_main(void)
{
    ESP_LOGI(TAG, "Waveshare status firmware started");

    while (true) {
        ESP_LOGI(TAG, "heartbeat");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
