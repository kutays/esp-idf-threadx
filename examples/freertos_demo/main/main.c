// SPDX-License-Identifier: Apache-2.0
/*
 * FreeRTOS on ESP32-C6 — Demo Application
 *
 * Standalone example showing two tasks interleaving on the standard
 * ESP-IDF FreeRTOS scheduler. Build and flash from this directory:
 *
 *   cd examples/freertos_demo
 *   idf.py build flash monitor
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "main";

static void blink_task(void *param)
{
    (void)param;
    int count = 0;
    while (1) {
        ESP_LOGI(TAG, "[blink] count=%d  tick=%lu",
                 count++, (unsigned long)xTaskGetTickCount());
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "FreeRTOS on ESP32-C6");

    xTaskCreate(blink_task, "blink", 4096, NULL, 5, NULL);

    int count = 0;
    while (1) {
        ESP_LOGI(TAG, "[main]  count=%d  tick=%lu",
                 count++, (unsigned long)xTaskGetTickCount());
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}
