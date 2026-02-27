/*
 * app_startup.c — ThreadX boot entry for the freertos override component.
 *
 * Provides esp_startup_start_app() which is called by esp_system/startup.c
 * after hardware initialization. We call tx_kernel_enter() directly —
 * no more port_start_app_hook() workaround.
 *
 * tx_application_define() remains in components/threadx/port/tx_port_startup.c
 * (creates system byte pool and main thread that calls app_main).
 */

#include "tx_api.h"
#include "esp_log.h"

static const char *TAG = "threadx_boot";

void esp_startup_start_app(void)
{
    ESP_LOGI(TAG, "=== ThreadX taking over (esp_startup_start_app) ===");

    /* tx_kernel_enter() does not return. It:
     *   1. Calls _tx_initialize_low_level() — installs vector table, sets up timer
     *   2. Calls tx_application_define()    — creates main_thread → app_main()
     *   3. Starts the ThreadX scheduler     — runs threads forever
     */
    tx_kernel_enter();

    /* Should never reach here */
    ESP_LOGE(TAG, "ERROR: tx_kernel_enter() returned!");
    while (1) { }
}
