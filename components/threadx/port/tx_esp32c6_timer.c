/*
 * tx_timer_interrupt.c — ESP32-C6 SYSTIMER tick for ThreadX
 *
 * Uses the same ESP-IDF HAL/ROM APIs that FreeRTOS uses for its own tick.
 * No raw register writes — all hardware access goes through proper APIs:
 *
 *   SYSTIMER hardware:  systimer_hal_* / systimer_ll_* (hal component)
 *   INTMTX routing:     esp_rom_route_intr_matrix()    (ROM, always available)
 *   PLIC configuration: esp_cpu_intr_set_*             (esp_hw_support component)
 *
 * Why we don't use esp_intr_alloc():
 *   esp_intr_alloc() internally calls portENTER_CRITICAL(&spinlock) which
 *   expects a spinlock-argument form of that macro. The upstream ThreadX
 *   FreeRTOS compat layer defines portENTER_CRITICAL() without arguments.
 *   To avoid that conflict, we call the underlying hardware functions directly
 *   (which is exactly what esp_intr_alloc does internally).
 *
 * Reference: compare with components/freertos/port_systick.c vSystimerSetup()
 */

#include "tx_api.h"
#include "tx_timer.h"

#include "soc/periph_defs.h"
#include "soc/interrupts.h"
#include "hal/systimer_hal.h"
#include "hal/systimer_ll.h"
#include "esp_cpu.h"
#include "esp_rom_sys.h"
#include "esp_private/systimer.h"
#include "esp_private/periph_ctrl.h"

/* _tx_timer_interrupt is the internal ThreadX tick handler.
   Not declared in any public ThreadX header. */
extern VOID _tx_timer_interrupt(VOID);

/* CPU interrupt priority (1–7 on ESP32-C6, must be > threshold=0) */
#define TIMER_CPU_INT_PRIORITY      1

/* CPU interrupt number to use for the tick.
 * We use a fixed number to avoid pulling in the full esp_intr_alloc machinery.
 * CPU interrupt 7 is free for application use on ESP32-C6. */
#define TIMER_CPU_INT_NUM           7

/* HAL context — kept alive for ISR use */
static systimer_hal_context_t s_systimer_hal;

/*
 * Global: CPU interrupt line used for the timer.
 * Read by the assembly trap handler to know which PLIC bit to check.
 */
UINT _tx_esp32c6_timer_cpu_int = TIMER_CPU_INT_NUM;

/*
 * _tx_port_setup_timer_interrupt
 *
 * Called from _tx_initialize_low_level (assembly) during ThreadX init.
 * Mirrors what FreeRTOS's vSystimerSetup() does.
 */
void _tx_port_setup_timer_interrupt(void)
{
    /* ------------------------------------------------------------------ */
    /* Step 1: Route SYSTIMER_TARGET0 peripheral source to CPU interrupt 7 */
    /* ------------------------------------------------------------------ */

    /* esp_rom_route_intr_matrix() is a ROM function that writes the INTMTX
     * map register for the given peripheral source, connecting it to the
     * chosen CPU interrupt line. Equivalent to:
     *   *(volatile uint32_t *)(0x60010000 + ETS_SYSTIMER_TARGET0_INTR_SOURCE*4) = TIMER_CPU_INT_NUM
     * but using the correct ROM API so it always stays correct. */
    esp_rom_route_intr_matrix(0, ETS_SYSTIMER_TARGET0_INTR_SOURCE, TIMER_CPU_INT_NUM);

    /* ------------------------------------------------------------------ */
    /* Step 2: Configure CPU interrupt line 7 via PLIC                    */
    /* ------------------------------------------------------------------ */

    /* Edge-triggered: the SYSTIMER alarm asserts a pulse.
     * We must clear the PLIC edge latch in the ISR via esp_cpu_intr_edge_ack(). */
    esp_cpu_intr_set_type(TIMER_CPU_INT_NUM, ESP_CPU_INTR_TYPE_EDGE);

    /* Set priority > threshold (default threshold=0) so the interrupt fires */
    esp_cpu_intr_set_priority(TIMER_CPU_INT_NUM, TIMER_CPU_INT_PRIORITY);

    /* Clear any stale edge latch before enabling */
    esp_cpu_intr_edge_ack(TIMER_CPU_INT_NUM);

    /* Enable CPU interrupt line 7 in the PLIC */
    esp_cpu_intr_enable(1u << TIMER_CPU_INT_NUM);

    /* ------------------------------------------------------------------ */
    /* Step 3: Configure SYSTIMER using the HAL (same as FreeRTOS does)   */
    /* ------------------------------------------------------------------ */

    /* Enable the SYSTIMER peripheral bus clock */
    PERIPH_RCC_ACQUIRE_ATOMIC(PERIPH_SYSTIMER_MODULE, ref_count) {
        if (ref_count == 0) {
            systimer_ll_enable_bus_clock(true);
            systimer_ll_reset_register();
        }
    }

    /* Initialise the HAL context (sets hal->dev to SYSTIMER hardware base) */
    systimer_hal_init(&s_systimer_hal);

    /* Provide tick↔µs conversion functions (SYSTIMER ticks at ~16 MHz on C6) */
    systimer_hal_tick_rate_ops_t ops = {
        .ticks_to_us = systimer_ticks_to_us,
        .us_to_ticks = systimer_us_to_ticks,
    };
    systimer_hal_set_tick_rate_ops(&s_systimer_hal, &ops);

    /* Reset counter 1 (SYSTIMER_COUNTER_OS_TICK) to zero */
    systimer_ll_set_counter_value(s_systimer_hal.dev, SYSTIMER_COUNTER_OS_TICK, 0);
    systimer_ll_apply_counter_value(s_systimer_hal.dev, SYSTIMER_COUNTER_OS_TICK);

    /* Connect alarm 0 (SYSTIMER_ALARM_OS_TICK_CORE0) to counter 1 */
    systimer_hal_connect_alarm_counter(&s_systimer_hal,
                                       SYSTIMER_ALARM_OS_TICK_CORE0,
                                       SYSTIMER_COUNTER_OS_TICK);

    /* Set the alarm period in microseconds.
     * TX_TIMER_TICKS_PER_SECOND = 100 → period = 10,000 µs = 10 ms */
    systimer_hal_set_alarm_period(&s_systimer_hal,
                                  SYSTIMER_ALARM_OS_TICK_CORE0,
                                  1000000UL / TX_TIMER_TICKS_PER_SECOND);

    /* Periodic mode: alarm auto-reloads after each period */
    systimer_hal_select_alarm_mode(&s_systimer_hal,
                                   SYSTIMER_ALARM_OS_TICK_CORE0,
                                   SYSTIMER_ALARM_MODE_PERIOD);

    /* Allow the counter to be stalled by the CPU debugger */
    systimer_hal_counter_can_stall_by_cpu(&s_systimer_hal,
                                           SYSTIMER_COUNTER_OS_TICK, 0, true);

    /* Enable the alarm interrupt in the SYSTIMER peripheral */
    systimer_hal_enable_alarm_int(&s_systimer_hal, SYSTIMER_ALARM_OS_TICK_CORE0);

    /* Start the counter */
    systimer_hal_enable_counter(&s_systimer_hal, SYSTIMER_COUNTER_OS_TICK);
}

/*
 * _tx_esp32c6_timer_isr
 *
 * Called from the assembly trap handler (_tx_esp32c6_trap_handler) when
 * CPU interrupt line 7 fires.
 *
 * Mirrors what FreeRTOS's SysTickIsrHandler() does for the hardware side.
 */
void _tx_esp32c6_timer_isr(void)
{
    /* Clear the SYSTIMER alarm interrupt (stops the peripheral asserting the line) */
    systimer_ll_clear_alarm_int(s_systimer_hal.dev, SYSTIMER_ALARM_OS_TICK_CORE0);

    /* Clear the PLIC edge latch for our CPU interrupt line.
     * Required for edge-triggered mode — without this the interrupt re-fires
     * immediately after mret. */
    esp_cpu_intr_edge_ack(TIMER_CPU_INT_NUM);

    /* Advance the ThreadX tick counter, wake sleeping threads */
    _tx_timer_interrupt();
}
