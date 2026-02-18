/*
 * tx_port_startup.c — ESP-IDF to ThreadX bridge
 *
 * How we hook into ESP-IDF startup:
 *
 *   FreeRTOS's esp_startup_start_app() is a STRONG symbol (not overrideable).
 *   It does this (from app_startup.c):
 *
 *     void esp_startup_start_app(void) {
 *         esp_int_wdt_init();
 *         esp_crosscore_int_init();
 *         xTaskCreatePinnedToCore(main_task, ...);   // creates FreeRTOS main task
 *         if (port_start_app_hook != NULL)
 *             port_start_app_hook();                 // ← WEAK hook we override
 *         vTaskStartScheduler();                     // FreeRTOS scheduler
 *     }
 *
 *   We provide a strong port_start_app_hook() that calls tx_kernel_enter()
 *   which never returns. vTaskStartScheduler() is therefore never reached.
 *   ThreadX owns the CPU from this point.
 *
 *   The FreeRTOS main_task that was created above never gets to run because
 *   the FreeRTOS scheduler never starts. Our own main_thread (created in
 *   tx_application_define) calls app_main() instead.
 *
 * Boot flow:
 *   ESP-IDF boot → esp_startup_start_app()
 *     → xTaskCreatePinnedToCore(main_task)  [created but never scheduled]
 *     → port_start_app_hook()               [our override]
 *       → tx_kernel_enter()                 [never returns]
 *         → _tx_initialize_low_level()
 *         → tx_application_define()         [creates our main_thread]
 *         → ThreadX scheduler starts
 *           → main_thread runs app_main()
 */

#include "tx_api.h"
#include "esp_log.h"

static const char *TAG = "threadx_startup";

/* Provided by the application (main.c) */
extern void app_main(void);

/* System byte pool for dynamic allocations */
#define SYSTEM_BYTE_POOL_SIZE   CONFIG_THREADX_BYTE_POOL_SIZE
static TX_BYTE_POOL system_byte_pool;
static UCHAR system_byte_pool_storage[SYSTEM_BYTE_POOL_SIZE];

/* Main thread — wraps app_main() */
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

    /* If app_main returns, suspend this thread — ThreadX keeps running */
    ESP_LOGW(TAG, "app_main() returned, suspending main thread");
    tx_thread_suspend(tx_thread_identify());
}

/*
 * tx_application_define
 *
 * Called by ThreadX during tx_kernel_enter(), before the scheduler starts.
 * Set up system resources and create the main application thread.
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
        MAIN_THREAD_PRIORITY,
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
 * port_start_app_hook
 *
 * Weak hook called by FreeRTOS's esp_startup_start_app() just before
 * vTaskStartScheduler(). By never returning from here, we prevent
 * FreeRTOS from starting and hand control to ThreadX instead.
 *
 * This is the correct interception point — esp_startup_start_app() itself
 * is a strong symbol in app_startup.c and cannot be overridden.
 */
void port_start_app_hook(void)
{
    ESP_LOGI(TAG, "=== ThreadX taking over (port_start_app_hook) ===");
    ESP_LOGI(TAG, "Entering ThreadX kernel — FreeRTOS scheduler will not start");

    /*
     * tx_kernel_enter() does not return. It:
     *   1. Calls _tx_initialize_low_level() — installs vector table, sets up timer
     *   2. Calls tx_application_define()   — creates our main_thread
     *   3. Starts the ThreadX scheduler    — runs threads forever
     */
    tx_kernel_enter();

    /* Should never reach here */
    ESP_LOGE(TAG, "ERROR: tx_kernel_enter() returned!");
    while (1) { }
}
