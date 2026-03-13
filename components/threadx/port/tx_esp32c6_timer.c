// SPDX-License-Identifier: Apache-2.0
/*
 * tx_esp32c6_timer.c — ESP32-C6 SYSTIMER tick for ThreadX
 *
 * PLIC configuration via direct register writes:
 *   The esp_cpu_intr_* API functions (which proxy to ROM esprv_intc_int_enable)
 *   were observed to NOT set the PLIC ENABLE bit for CPU interrupt 7 (old design).
 *   We bypass the API and write PLIC registers directly using the addresses from
 *   soc/plic_reg.h (DR_REG_PLIC_MX_BASE = 0x20001000).
 *
 *   Register addresses (TIMER_CPU_INT_NUM = 17):
 *     PLIC_MXINT_ENABLE_REG   = 0x20001000 + 0x00  (bitmask: bit N enables line N)
 *     PLIC_MXINT_TYPE_REG     = 0x20001000 + 0x04  (bitmask: bit N=1 → edge, 0 → level)
 *     PLIC_MXINT_CLEAR_REG    = 0x20001000 + 0x08  (write bit N to ack edge latch)
 *     PLIC_EMIP_STATUS_REG    = 0x20001000 + 0x0C  (read: which edge latches are set)
 *     PLIC_MXINT17_PRI_REG    = 0x20001000 + 0x54  (4-bit priority for line 17)
 *     PLIC_MXINT_THRESH_REG   = 0x20001000 + 0x90  (lines with pri <= thresh masked)
 *
 *   FreeRTOS startup sets threshold = 1 (RVHAL_INTR_ENABLE_THRESH). Our priority
 *   must be STRICTLY GREATER than the threshold, so we use priority 2.
 *
 *   LEVEL-TRIGGERED (matches FreeRTOS vSystimerSetup, no ESP_INTR_FLAG_EDGE):
 *   We use level-triggered mode (TYPE bit 17 = 0) for SYSTIMER, matching the
 *   FreeRTOS port_systick.c approach. With level-triggered:
 *     - While SYSTIMER INT_ST=1, PLIC continuously asserts mip.bit17
 *     - CPU takes interrupt → ISR clears INT_ST via systimer_ll_clear_alarm_int()
 *     - Clearing INT_ST drops the SYSTIMER output → PLIC deasserts mip.bit17
 *     - 10ms later: next alarm fires → INT_ST=1 → interrupt fires again
 *   Edge-triggered was previously used but caused issues because: if the alarm
 *   had already fired (INT_ST=1, output=HIGH) when PLIC edge detection was
 *   configured, there was no 0→1 transition for the PLIC to latch, so the
 *   interrupt was never delivered.
 *
 *   mideleg CSR bit 17 must be 0 so interrupt 17 is handled in machine mode.
 *   esp_intr_alloc() does "RV_CLEAR_CSR(mideleg, BIT(intr))" for exactly this.
 *
 * SYSTIMER hardware: configured via systimer_hal_* / systimer_ll_* (HAL).
 * INTMTX routing:   via esp_rom_route_intr_matrix() (ROM).
 *
 * Reference: components/freertos/port_systick.c vSystimerSetup()
 *            components/esp_hw_support/intr_alloc.c esp_intr_alloc()
 *            components/soc/esp32c6/register/soc/plic_reg.h
 *            components/soc/esp32c6/register/soc/reg_base.h
 */

#include "tx_api.h"
#include "tx_timer.h"

#include "soc/periph_defs.h"
#include "soc/interrupts.h"
#include "hal/systimer_hal.h"
#include "hal/systimer_ll.h"
#include "esp_rom_sys.h"
#include "esp_private/systimer.h"
#include "esp_private/periph_ctrl.h"
#include "esp_log.h"

/* _tx_timer_interrupt is the internal ThreadX tick handler.
   Not declared in any public ThreadX header. */
extern VOID _tx_timer_interrupt(VOID);

/* CPU interrupt number to use for the tick.
 * Lines 1, 3, 4, 6, 7 are reserved on ESP32-C6 (CLINT/Wi-Fi bound).
 * Lines 6, 7, 11, 15, 16, 29 are "effectively disabled" by the INTMTX hardware.
 * Line 17 is free for application use. */
#define TIMER_CPU_INT_NUM           17

/* Priority for our CPU interrupt line.
 * FreeRTOS startup sets PLIC threshold = 1 (RVHAL_INTR_ENABLE_THRESH = 1).
 * Only interrupts with priority STRICTLY GREATER than threshold fire.
 * We use 2 to stay above the threshold. */
#define TIMER_CPU_INT_PRIORITY      2

/* PLIC MX register addresses (DR_REG_PLIC_MX_BASE = 0x20001000) */
#define PLIC_MX_BASE        0x20001000UL
#define PLIC_REG(offset)    (*(volatile uint32_t *)(PLIC_MX_BASE + (offset)))
#define PLIC_MX_ENABLE      PLIC_REG(0x00)
#define PLIC_MX_TYPE        PLIC_REG(0x04)
#define PLIC_MX_CLEAR       PLIC_REG(0x08)
#define PLIC_MX_EMIP        PLIC_REG(0x0C)  /* Edge Machine Interrupt Pending */
#define PLIC_MX_PRI_N       PLIC_REG(0x10 + TIMER_CPU_INT_NUM * 4)  /* PRI[N] */
#define PLIC_MX_THRESH      PLIC_REG(0x90)

/* HAL context — kept alive for ISR use */
static systimer_hal_context_t s_systimer_hal;

/* CPU interrupt line used for the timer. */
UINT _tx_esp32c6_timer_cpu_int = TIMER_CPU_INT_NUM;

/* Diagnostic counter — incremented every time the ISR C function is entered.
 * If this stays 0, the ISR is never reaching C (mtvec/PLIC/SYSTIMER problem).
 * If it advances, the ISR fires but something else prevents tick from advancing. */
volatile uint32_t g_tx_timer_isr_count = 0;

/*
 * _tx_port_setup_timer_interrupt
 *
 * Called from _tx_initialize_low_level (assembly) during ThreadX init.
 */
void _tx_port_setup_timer_interrupt(void)
{
    /* ------------------------------------------------------------------ */
    /* Step 1: Route SYSTIMER_TARGET0 peripheral source to CPU interrupt 17 */
    /* ------------------------------------------------------------------ */
    esp_rom_route_intr_matrix(0, ETS_SYSTIMER_TARGET0_INTR_SOURCE, TIMER_CPU_INT_NUM);

    /* ------------------------------------------------------------------ */
    /* Step 2: Configure SYSTIMER using the HAL (same as FreeRTOS does)   */
    /* ------------------------------------------------------------------ */

    /* The SYSTIMER peripheral bus clock is already enabled by ESP-IDF's esp_timer
     * init (ESP_SYSTEM_INIT_FN priority 101, runs during do_core_init()).
     *
     * We intentionally do NOT call systimer_ll_enable_bus_clock() or
     * PERIPH_RCC_ACQUIRE_ATOMIC here because:
     * 1. systimer_ll_enable_bus_clock() is wrapped in a macro requiring
     *    __DECLARE_RCC_RC_ATOMIC_ENV (the PERIPH_RCC critical-section env).
     * 2. PERIPH_RCC_ACQUIRE_ATOMIC uses portENTER/EXIT_CRITICAL, and during init
     *    vPortExitCritical() re-enables mstatus.MIE when nesting count reaches 0.
     *    This lets the SYSTIMER interrupt fire before any threads exist, causing
     *    _tx_thread_context_restore to hang in its idle loop (same as Bug 29).
     * 3. The bus clock is already on — no action needed.
     *
     * Do NOT reset SYSTIMER registers — esp_timer configured other alarms/counters
     * during ESP-IDF CORE init that we must not disturb. */

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

    /* ------------------------------------------------------------------ */
    /* Step 3: Configure CPU interrupt line 17 via direct PLIC registers  */
    /* ------------------------------------------------------------------ */

    /* NOTE: PLIC threshold is NOT set here. The default threshold from
     * ESP-IDF startup is 1 (RVHAL_INTR_ENABLE_THRESH). Our SYSTIMER uses
     * priority 2 (> threshold 1), so it fires regardless.
     *
     * For WiFi/BLE demos that need ESP-IDF interrupts (allocated at priority
     * 1 by esp_intr_alloc), the custom FreeRTOS component sets PLIC threshold
     * to 0 via _tx_port_esp_idf_isr_init(). This keeps the timer code
     * decoupled from port-level interrupt policy. */

    /* 3a. LEVEL-triggered mode (matches FreeRTOS vSystimerSetup):
     *     Leave TYPE bit 17 = 0 (level). Do NOT set it to edge.
     *     With level-triggered: PLIC asserts mip.bit17 continuously while
     *     SYSTIMER INT_ST=1. ISR clears INT_ST → output drops → PLIC deasserts.
     *     Edge-triggered caused problems because the PLIC may miss the rising
     *     edge if the output is already HIGH when the PLIC is enabled/configured. */
    PLIC_MX_TYPE &= ~(1u << TIMER_CPU_INT_NUM);   /* clear bit 17 = level */

    /* 3b. Priority 2 — must be > PLIC threshold (default 1, or 0 if wifi port) */
    PLIC_MX_PRI_N = TIMER_CPU_INT_PRIORITY;

    /* 3c. Ensure interrupt 17 is handled in machine mode (not delegated).
     *     esp_intr_alloc() does this same CSR write for PLIC targets. */
    __asm__ volatile("csrc mideleg, %0" :: "r"(1u << TIMER_CPU_INT_NUM));

    /* 3d. Enable CPU interrupt line 17 in the PLIC ENABLE register */
    PLIC_MX_ENABLE |= (1u << TIMER_CPU_INT_NUM);

    /* 3e. Set mie CSR bit 17.
     *     On ESP32-C6 the PLIC requires BOTH PLIC_ENABLE bit N AND mie bit N to be
     *     set for interrupt line N to fire.  The ROM esprv_intc_int_enable() does
     *     "csrs mie, mask" internally; our direct PLIC write only set PLIC_ENABLE.
     *     FreeRTOS working lines (2,5,8,25,27) all have their bits in BOTH registers.
     *     We already set mie bit 11 (MEIE) in _tx_initialize_low_level; here we set
     *     the per-line mie bit for line 17. */
    __asm__ volatile("csrs mie, %0" :: "r"(1u << TIMER_CPU_INT_NUM));

    /* ------------------------------------------------------------------ */
    /* Diagnostic: read back hardware registers to confirm configuration.  */
    /* Remove once timer is confirmed working.                             */
    /* ------------------------------------------------------------------ */
    uint32_t mtvec_val, mie_val, mip_val, mideleg_val, mstatus_val;
    __asm__ volatile("csrr %0, mtvec"   : "=r"(mtvec_val));
    __asm__ volatile("csrr %0, mie"     : "=r"(mie_val));
    __asm__ volatile("csrr %0, mip"     : "=r"(mip_val));
    __asm__ volatile("csrr %0, mideleg" : "=r"(mideleg_val));
    __asm__ volatile("csrr %0, mstatus" : "=r"(mstatus_val));

    ESP_LOGI("tx_diag", "--- Timer HW State ---");
    ESP_LOGI("tx_diag", "mtvec          = 0x%08lx  (should match vector_table addr)", (uint32_t)mtvec_val);
    ESP_LOGI("tx_diag", "mstatus        = 0x%08lx  (bit3 MIE, bit7 MPIE, bits12:11 MPP)", (uint32_t)mstatus_val);
    ESP_LOGI("tx_diag", "mie            = 0x%08lx  (bit11 MEIE must be 1 = 0x800)", (uint32_t)mie_val);
    ESP_LOGI("tx_diag", "mip            = 0x%08lx  (bit17 pending = CPU sees interrupt)", (uint32_t)mip_val);
    ESP_LOGI("tx_diag", "mideleg        = 0x%08lx  (bit17 must be 0)", (uint32_t)mideleg_val);
    ESP_LOGI("tx_diag", "INTMTX src57   = 0x%08lx  (must be %d = cpu_int)", *(volatile uint32_t *)(0x60010000 + 57*4), TIMER_CPU_INT_NUM);
    ESP_LOGI("tx_diag", "PLIC ENABLE    = 0x%08lx  (bit17 must be set = 0x20000)", PLIC_MX_ENABLE);
    ESP_LOGI("tx_diag", "PLIC TYPE      = 0x%08lx  (bit17 must be 0 = level-triggered)", PLIC_MX_TYPE);
    ESP_LOGI("tx_diag", "PLIC EMIP      = 0x%08lx  (edge-pending: bit17 only set if edge-mode)", PLIC_MX_EMIP);
    ESP_LOGI("tx_diag", "PLIC PRI[17]   = 0x%08lx  (must be > THRESH)", PLIC_MX_PRI_N);
    ESP_LOGI("tx_diag", "PLIC THRESH    = 0x%08lx  (must be < PRI[17])", PLIC_MX_THRESH);
    /* Read SYSTIMER registers via the HAL dev pointer (base = 0x6000A000 on C6).
     * Register map (from systimer_reg.h):
     *   0x00 CONF:          bit31=clk_en, bit29=ctr1_en, bit30=ctr0_en, bit26=ctr1_stall, bit24=alm0_en
     *   0x34 TARGET0_CONF:  bits[25:0]=period_ticks, bit30=period_mode, bit31=unit_sel(1→ctr1)
     *   0x64 INT_ENA:       bit0=alarm0 int enabled
     *   0x70 INT_ST:        bit0=alarm0 currently pending
     * Expected after setup: CONF≈0xF7000002, TGT0≈0xC0027100 (160000 ticks,period,ctr1), INT_ENA≈0x1 */
    volatile uint32_t *_st = (volatile uint32_t *)s_systimer_hal.dev;
    ESP_LOGI("tx_diag", "SYSTIMER base  = 0x%08lx  (expect 0x6000A000)", (uint32_t)s_systimer_hal.dev);
    ESP_LOGI("tx_diag", "SYSTIMER_CONF  = 0x%08lx  (bit31 clk_en, bit29 ctr1_en, bit24 alm0_en)",
             _st[0x00/4]);
    ESP_LOGI("tx_diag", "SYSTIMER TGT0  = 0x%08lx  (bit31=ctr_sel, bit30=period, [25:0]=ticks)",
             _st[0x34/4]);   /* TARGET0_CONF_REG at offset 0x34 */
    ESP_LOGI("tx_diag", "SYSTIMER INTENA= 0x%08lx  (bit0=alm0 int enabled)",
             _st[0x64/4]);   /* INT_ENA_REG at offset 0x64 */
    ESP_LOGI("tx_diag", "SYSTIMER INT_ST= 0x%08lx  (bit0=alm0 currently pending)",
             _st[0x70/4]);   /* INT_ST_REG at offset 0x70 */
    ESP_LOGI("tx_diag", "----------------------");
}

/*
 * _tx_esp32c6_timer_isr
 *
 * Called from the assembly trap handler (_tx_esp32c6_trap_handler) when
 * CPU interrupt line 17 fires (SYSTIMER alarm 0, level-triggered).
 *
 * For LEVEL-triggered operation:
 *   1. Clear SYSTIMER INT_ST — this drops the SYSTIMER output LOW,
 *      which causes the PLIC to deassert mip.bit17 automatically.
 *   2. No PLIC CLEAR write needed (that is only for edge-triggered mode).
 *   The PLIC will reassert after the next alarm fires 10ms later.
 *
 * This matches the FreeRTOS SysTickIsrHandler approach in port_systick.c.
 */
void _tx_esp32c6_timer_isr(void)
{
    g_tx_timer_isr_count++;   /* diagnostic: proof of ISR entry */

    /* Clear the SYSTIMER alarm interrupt.
     * This drops the SYSTIMER output LOW → PLIC deasserts mip.bit17 automatically.
     * (No PLIC CLEAR needed — level-triggered mode, not edge-triggered.) */
    systimer_ll_clear_alarm_int(s_systimer_hal.dev, SYSTIMER_ALARM_OS_TICK_CORE0);

    /* Advance the ThreadX tick counter, wake sleeping threads */
    _tx_timer_interrupt();
}
