/*
 * ThreadX on ESP32-C6 — Native ThreadX API Demo
 *
 * Demonstrates two ThreadX threads interleaving via tx_thread_sleep().
 * Uses the native ThreadX API directly (tx_thread_create, tx_thread_sleep,
 * tx_time_get). Requires RTOS_SELECTION_THREADX in menuconfig.
 *
 * Build and flash:
 *   cd examples/threadx_demo
 *   idf.py build flash monitor
 */

#include "tx_api.h"
#include "esp_log.h"

/* Diagnostic counter incremented by the SYSTIMER ISR each tick */
extern volatile uint32_t g_tx_timer_isr_count;

static const char *TAG = "main";

#define BLINK_STACK_SIZE    4096
#define BLINK_THREAD_PRIO   17

static TX_THREAD blink_thread;
static UCHAR blink_thread_stack[BLINK_STACK_SIZE];

static void blink_thread_entry(ULONG param)
{
    (void)param;
    uint32_t ms_val;
    __asm__ volatile("csrr %0, mstatus" : "=r"(ms_val));
    ESP_LOGI(TAG, "[blink] mstatus=0x%08lx (bit3 MIE must be 1)", (unsigned long)ms_val);
    ULONG count = 0;

    while (1) {
        ESP_LOGI(TAG, "[blink] tick=%lu  isr=%lu  count=%lu",
                 tx_time_get(), (ULONG)g_tx_timer_isr_count, count++);
        tx_thread_sleep(10);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "============================================");
    ESP_LOGI(TAG, "  ThreadX on ESP32-C6 — Native API Demo");
    ESP_LOGI(TAG, "============================================");
    ESP_LOGI(TAG, "ThreadX version: %u", (unsigned)THREADX_MAJOR_VERSION);
    ESP_LOGI(TAG, "Tick rate: %d Hz", TX_TIMER_TICKS_PER_SECOND);

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
        return;
    }

    ULONG count = 0;
    while (1) {
        ESP_LOGI(TAG, "[main]  tick=%lu  isr=%lu  count=%lu",
                 tx_time_get(), (ULONG)g_tx_timer_isr_count, count++);
        tx_thread_sleep(20);
    }
}
