/*
 * tx_port_startup.c — ESP-IDF to ThreadX bridge
 *
 * This file provides the startup bridge between ESP-IDF's boot sequence
 * and the ThreadX kernel. ESP-IDF calls esp_startup_start_app() after
 * hardware init is complete. We provide a strong symbol that overrides
 * ESP-IDF's weak default (which normally starts the FreeRTOS scheduler).
 *
 * Flow:
 *   ESP-IDF boot → start_cpu0_default() → esp_startup_start_app()
 *     → [our override] → tx_kernel_enter()
 *       → _tx_initialize_low_level()
 *       → tx_application_define()
 *       → ThreadX scheduler starts
 */

#include "tx_api.h"
#include "esp_log.h"

static const char *TAG = "threadx_startup";

/* Provided by the application (main.c) */
extern void app_main(void);

/* System byte pool for dynamic allocations */
#define SYSTEM_BYTE_POOL_SIZE   (32 * 1024)
static TX_BYTE_POOL system_byte_pool;
static UCHAR system_byte_pool_storage[SYSTEM_BYTE_POOL_SIZE];

/* Main thread — runs app_main() */
#define MAIN_THREAD_STACK_SIZE  4096
#define MAIN_THREAD_PRIORITY    16
static TX_THREAD main_thread;
static UCHAR main_thread_stack[MAIN_THREAD_STACK_SIZE];

/* Global reference to system byte pool for compat layer */
TX_BYTE_POOL *_tx_esp32c6_system_byte_pool = NULL;

static void main_thread_entry(ULONG param)
{
    (void)param;
    ESP_LOGI(TAG, "Main thread started, calling app_main()");
    app_main();

    /* If app_main returns, just suspend this thread */
    ESP_LOGW(TAG, "app_main() returned, suspending main thread");
    tx_thread_suspend(tx_thread_identify());
}

/*
 * tx_application_define
 *
 * Called by ThreadX during tx_kernel_enter(), before the scheduler starts.
 * This is the standard ThreadX hook where we set up system resources
 * and create the main application thread.
 *
 * first_unused_memory: pointer to first free memory after ThreadX data
 */
void tx_application_define(void *first_unused_memory)
{
    UINT status;

    (void)first_unused_memory;

    ESP_LOGI(TAG, "tx_application_define: setting up system resources");

    /* Create system byte pool for dynamic allocations */
    status = tx_byte_pool_create(
        &system_byte_pool,
        "system_pool",
        system_byte_pool_storage,
        SYSTEM_BYTE_POOL_SIZE
    );
    if (status != TX_SUCCESS) {
        ESP_LOGE(TAG, "Failed to create system byte pool: %u", status);
        return;
    }
    _tx_esp32c6_system_byte_pool = &system_byte_pool;

    /* Create the main application thread */
    status = tx_thread_create(
        &main_thread,
        "main",
        main_thread_entry,
        0,
        main_thread_stack,
        MAIN_THREAD_STACK_SIZE,
        MAIN_THREAD_PRIORITY,
        MAIN_THREAD_PRIORITY,    /* preemption threshold = priority (no threshold) */
        TX_NO_TIME_SLICE,
        TX_AUTO_START
    );
    if (status != TX_SUCCESS) {
        ESP_LOGE(TAG, "Failed to create main thread: %u", status);
        return;
    }

    ESP_LOGI(TAG, "ThreadX application defined — main thread created");
}

/*
 * esp_startup_start_app
 *
 * Strong symbol override of ESP-IDF's weak default.
 * Called at the end of the ESP-IDF startup sequence.
 * Instead of starting FreeRTOS, we enter the ThreadX kernel.
 */
void esp_startup_start_app(void)
{
    ESP_LOGI(TAG, "=== ThreadX taking over from ESP-IDF startup ===");
    ESP_LOGI(TAG, "Entering ThreadX kernel...");

    /*
     * tx_kernel_enter() does not return. It:
     *   1. Calls _tx_initialize_low_level() — our asm init
     *   2. Calls tx_application_define() — our function above
     *   3. Starts the ThreadX scheduler
     */
    tx_kernel_enter();

    /* Should never reach here */
    ESP_LOGE(TAG, "ERROR: tx_kernel_enter() returned!");
    while (1) {
        /* spin */
    }
}
