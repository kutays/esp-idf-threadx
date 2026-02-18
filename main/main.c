/*
 * ThreadX on ESP32-C6 — Demo Application
 *
 * This is the main application entry point. ThreadX calls
 * tx_application_define() during kernel init, which sets up a byte pool
 * and creates the demo thread. The demo thread then calls app_main().
 */

#include "tx_api.h"
#include "esp_log.h"

static const char *TAG = "main";

/* Second test thread */
#define BLINK_STACK_SIZE    4096
//#define BLINK_THREAD_PRIO   20
#define BLINK_THREAD_PRIO   5

static TX_THREAD blink_thread;
static UCHAR blink_thread_stack[BLINK_STACK_SIZE];

static void blink_thread_entry(ULONG param)
{
    (void)param;
    ULONG count = 0;

    while (1) {
        ESP_LOGI(TAG, "[blink] tick=%lu  count=%lu", tx_time_get(), count++);
        tx_thread_sleep(100);  /* ~1 second at 100 Hz */
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "============================================");
    ESP_LOGI(TAG, "  ThreadX on ESP32-C6 — Demo Application");
    ESP_LOGI(TAG, "============================================");
    ESP_LOGI(TAG, "ThreadX version: %u", (unsigned)THREADX_MAJOR_VERSION);
    ESP_LOGI(TAG, "Tick rate: %d Hz", TX_TIMER_TICKS_PER_SECOND);

    /* Create a second thread to show multi-threading works */
    UINT status = tx_thread_create(
        &blink_thread,
        "blink",
        blink_thread_entry,
        0,
        blink_thread_stack,
        BLINK_STACK_SIZE,
        BLINK_THREAD_PRIO,
        BLINK_THREAD_PRIO,
        TX_NO_TIME_SLICE,
        TX_AUTO_START
    );

    if (status != TX_SUCCESS) {
        ESP_LOGE(TAG, "Failed to create blink thread: %u", status);
    }

        ESP_LOGE(TAG, "Thread created: %u", status);
    /* Main thread loop */
    ULONG count = 0;
    
    while (1) {
        ESP_LOGI(TAG, "[main]  tick=%lu  count=%lu", tx_time_get(), count++);
        tx_thread_sleep(200);  /* ~2 seconds */
    }
    
}
