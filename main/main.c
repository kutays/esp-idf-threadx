/*
 * ThreadX on ESP32-C6 — Demo Application
 *
 * This is the main application entry point. ThreadX calls
 * tx_application_define() during kernel init, which sets up a byte pool
 * and creates the demo thread. The demo thread then calls app_main().
 */

#include "tx_api.h"
#include "esp_log.h"

/* Diagnostic counter defined in tx_esp32c6_timer.c */
extern volatile uint32_t g_tx_timer_isr_count;

static const char *TAG = "main";

/* Second test thread */
#define BLINK_STACK_SIZE    4096
#define BLINK_THREAD_PRIO   17
//#define BLINK_THREAD_PRIO   5

static TX_THREAD blink_thread;
static UCHAR blink_thread_stack[BLINK_STACK_SIZE];

static void blink_thread_entry(ULONG param)
{
    (void)param;
    uint32_t ms_val;
    __asm__ volatile("csrr %0, mstatus" : "=r"(ms_val));
    ESP_LOGI(TAG, "[blink] mstatus at thread start = 0x%08lx  (bit3 MIE must be 1)", (unsigned long)ms_val);
    ULONG count = 0;

    while (1) {
        /* Busy-wait ~50ms using a raw spin loop (no sleep) so we keep printing
         * even if tx_thread_sleep is broken. Spin count tuned for ~160 MHz. */
//        for (volatile uint32_t i = 0; i < 2000000UL; i++);
        ESP_LOGI(TAG, "[blink] tick=%lu  isr_count=%lu  count=%lu",
                 tx_time_get(), (ULONG)g_tx_timer_isr_count, count++);
	tx_thread_sleep(10);
//        tx_thread_relinquish();   /* yield — let main run */
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
    /* Main thread loop — busy-wait, no sleep, so we keep printing
     * regardless of whether the tick timer works. */
    ULONG count = 0;
    while (1) {
        //for (volatile uint32_t i = 0; i < 4000000UL; i++);
        ESP_LOGI(TAG, "[main]  tick=%lu  isr_count=%lu  count=%lu",
                 tx_time_get(), (ULONG)g_tx_timer_isr_count, count++);
	tx_thread_sleep(20);
        //tx_thread_relinquish();
    }
    
}
