/*
 * This file is a placeholder. The application demos live under examples/:
 *
 *   ThreadX native API demo:
 *     cd examples/threadx_demo && idf.py build flash monitor
 *
 *   FreeRTOS demo (also runs via ThreadX compat layer if RTOS_SELECTION_THREADX):
 *     cd examples/freertos_demo && idf.py build flash monitor
 *
 * Each example has its own sdkconfig.defaults and can be independently
 * configured via idf.py menuconfig from within its directory.
 */

#include "esp_log.h"

void app_main(void)
{
    esp_log_write(ESP_LOG_WARN, "main",
        "Build from examples/threadx_demo or examples/freertos_demo\n");
}
