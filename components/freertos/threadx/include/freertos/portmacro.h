// SPDX-License-Identifier: Apache-2.0
/*
 * portmacro.h — ESP32-C6 RISC-V + ThreadX port macros
 *
 * Provides ESP-IDF-specific types and macros that the upstream ThreadX
 * FreeRTOS compat layer doesn't define:
 *   - portMUX_TYPE (spinlock — degenerates to interrupt disable on single-core)
 *   - portENTER_CRITICAL(mux) with spinlock argument
 *   - xPortInIsrContext(), xPortCanYield()
 *   - Queue position macros (queueSEND_TO_BACK, etc.)
 *   - Core affinity macros (tskNO_AFFINITY, etc.)
 */

#ifndef PORTMACRO_H
#define PORTMACRO_H

#include <stdint.h>
#include <stdbool.h>

/* ── Transitive includes matching real FreeRTOS portmacro.h ──────
 *
 * Real portmacro.h includes: spinlock.h, soc/interrupt_reg.h, esp_macros.h,
 * esp_attr.h, esp_cpu.h, esp_rom_sys.h, esp_heap_caps.h, esp_system.h,
 * esp_newlib.h, esp_timer.h.
 *
 * Many ESP-IDF components rely on these transitive includes without
 * including the headers directly. We must match the key ones:
 *
 *   esp_heap_caps.h — heap_caps_malloc() used by esp_https_ota, esp_coex, etc.
 *   soc/soc_caps.h  — SOC_WIFI_LIGHT_SLEEP_CLK_WIDTH used by esp_coex_adapter.c
 *                     Chain in real FreeRTOS: portmacro.h → spinlock.h →
 *                     riscv/rv_utils.h → soc/soc_caps.h
 *
 * We include these directly rather than the full chain (spinlock.h pulls in
 * rv_utils.h which has RISC-V CSR helpers we don't need).
 */
#include "esp_heap_caps.h"
#include "soc/soc_caps.h"
#include "esp_rom_sys.h"

/* ── Base type definitions ─────────────────────────────────────────
 * Normally provided by the upstream compat FreeRTOS.h. But some ESP-IDF
 * components (ieee802154, esp_phy) include portmacro.h standalone without
 * FreeRTOS.h. Provide the types here when that happens (FREERTOS_H is the
 * upstream compat header's include guard). */
#ifndef FREERTOS_H
typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;
typedef uint8_t StackType_t;
#define pdTRUE   ((BaseType_t) 1)
#define pdFALSE  ((BaseType_t) 0)
#define portMAX_DELAY  ((TickType_t) 0xffffffffUL)
#endif

/* ── Type definitions ──────────────────────────────────────────────
 * When FreeRTOS.h IS included, BaseType_t etc. come from the upstream compat.
 * Do NOT redefine here
 * to avoid conflicting typedef errors (UINT vs uint32_t on RISC-V).
 */

/* Tick conversion */
#define portTICK_PERIOD_MS      ((TickType_t)(1000 / configTICK_RATE_HZ))

/* Stack */
#define portBYTE_ALIGNMENT      16
#define portSTACK_GROWTH        (-1)

/* ── Spinlock / portMUX_TYPE ─────────────────────────────────────
 * Include ESP-IDF's real spinlock.h rather than defining our own spinlock_t.
 * This prevents type conflicts when ESP-IDF files include both our portmacro.h
 * and the real spinlock.h (e.g., vfs_eventfd.c).
 *
 * On single-core ESP32-C6 (CONFIG_ESP_SYSTEM_SINGLE_CORE_MODE=y), spinlock
 * acquire/release are trivial no-ops. Our Kconfig sets FREERTOS_UNICORE which
 * selects ESP_SYSTEM_SINGLE_CORE_MODE.
 */
#include "spinlock.h"

typedef spinlock_t portMUX_TYPE;

#define portMUX_INITIALIZER_UNLOCKED SPINLOCK_INITIALIZER
#define portMUX_FREE_VAL            SPINLOCK_FREE

/* portMUX_INITIALIZE — macro alias used by ESP-IDF drivers (parlio, etc.) */
#define portMUX_INITIALIZE(mux)  spinlock_initialize(mux)

/* ── Critical sections WITH spinlock argument ────────────────────
 * Single-core: mux is ignored, just disable/enable interrupts.
 */
extern void vPortEnterCritical(void);
extern void vPortExitCritical(void);

#define portENTER_CRITICAL(mux)         do { (void)(mux); vPortEnterCritical(); } while(0)
#define portEXIT_CRITICAL(mux)          do { (void)(mux); vPortExitCritical(); } while(0)
#define portENTER_CRITICAL_ISR(mux)     portENTER_CRITICAL(mux)
#define portEXIT_CRITICAL_ISR(mux)      portEXIT_CRITICAL(mux)
#define portTRY_ENTER_CRITICAL(mux, t)  ({ (void)(mux); (void)(t); vPortEnterCritical(); 1; })
#define portTRY_ENTER_CRITICAL_ISR(mux, t) portTRY_ENTER_CRITICAL(mux, t)

/* taskENTER/EXIT_CRITICAL use mux=NULL on ESP-IDF */
#define taskENTER_CRITICAL()            portENTER_CRITICAL(NULL)
#define taskEXIT_CRITICAL()             portEXIT_CRITICAL(NULL)

/* SAFE variants — auto-detect ISR vs task context.
 * On single-core both paths do the same thing (disable interrupts). */
#define portENTER_CRITICAL_SAFE(mux)    portENTER_CRITICAL(mux)
#define portEXIT_CRITICAL_SAFE(mux)     portEXIT_CRITICAL(mux)

/* ── ISR context detection ───────────────────────────────────────
 * Uses ThreadX _tx_thread_system_state to detect ISR context.
 */
extern BaseType_t xPortInIsrContext(void);

static inline bool __attribute__((always_inline))
xPortCanYield(void) { return !xPortInIsrContext(); }

/* ── Yield ───────────────────────────────────────────────────────── */
extern void vPortYield(void);
#define portYIELD()                     vPortYield()

/* Variadic: supports both portYIELD_FROM_ISR() and portYIELD_FROM_ISR(x)
 *
 * Bug 35: These were originally defined as vPortYield() → tx_thread_relinquish().
 * That is a THREAD-LEVEL ThreadX API — calling it from ISR context corrupts the
 * scheduler (modifies _tx_thread_current_ptr while ISR context save/restore
 * expects it stable).
 *
 * Fix: No-op. rtos_int_exit calls _tx_thread_system_preempt_check() when
 * exiting the last ISR nesting level. This checks _tx_thread_execute_ptr
 * (updated by tx_semaphore_put etc. during the ISR) against _tx_thread_current_ptr.
 * If a higher-priority thread became ready, it triggers _tx_thread_system_return()
 * which saves a solicited context frame and enters the ThreadX scheduler. No
 * xPortSwitchFlag flag is needed — ThreadX's own execute_ptr handles everything.
 */
#undef portYIELD_FROM_ISR
#undef portEND_SWITCHING_ISR
#define portYIELD_FROM_ISR(...)         ((void)0)
#define portEND_SWITCHING_ISR(...)      ((void)0)

/* ── NOP / memory barrier ────────────────────────────────────────── */
#define portNOP()                       __asm__ volatile("nop")
#define portMEMORY_BARRIER()            __asm__ volatile("fence" ::: "memory")

/* ── ESP-IDF specifics ───────────────────────────────────────────── */
#define portNUM_PROCESSORS              1
#define xPortGetCoreID()                0
#define tskNO_AFFINITY                  ((UBaseType_t)-1)

#define CONFIG_FREERTOS_NUMBER_OF_CORES 1

/* Queue position macros — used by esp_adapter.c xQueueGenericSend */
#define queueSEND_TO_BACK               ((BaseType_t)0)
#define queueSEND_TO_FRONT              ((BaseType_t)1)
#define queueOVERWRITE                  ((BaseType_t)2)

/* ── Interrupt mask (ISR-safe interrupt control) ──────────────────
 * Real portmacro.h defines these for save/restore interrupt state.
 * Used by portDISABLE/ENABLE_INTERRUPTS and ISR-safe critical sections. */
#define portDISABLE_INTERRUPTS()        do { __asm__ volatile("csrc mstatus, 8"); } while(0)
#define portENABLE_INTERRUPTS()         do { __asm__ volatile("csrs mstatus, 8"); } while(0)

static inline unsigned __attribute__((always_inline))
xPortSetInterruptMaskFromISR(void) {
    unsigned prev;
    __asm__ volatile("csrrc %0, mstatus, 8" : "=r"(prev));
    return prev;
}
static inline void __attribute__((always_inline))
vPortClearInterruptMaskFromISR(unsigned prev) {
    __asm__ volatile("csrs mstatus, %0" :: "r"(prev & 0x8));
}

#define portSET_INTERRUPT_MASK_FROM_ISR()               xPortSetInterruptMaskFromISR()
#define portCLEAR_INTERRUPT_MASK_FROM_ISR(prev_level)   vPortClearInterruptMaskFromISR(prev_level)

/* ── Miscellaneous port macros used by ESP-IDF components ─────── */
#define portCHECK_IF_IN_ISR()           xPortInIsrContext()
#define portASSERT_IF_IN_ISR()          /* no-op on single-core */
#define portYIELD_WITHIN_API()          portYIELD()
#define portGET_CORE_ID()               ((BaseType_t)0)
#define portVALID_LIST_MEM(ptr)         (true)
#define portVALID_TCB_MEM(ptr)          (true)
#define portVALID_STACK_MEM(ptr)        (true)
#define portTICK_TYPE_IS_ATOMIC         1
#define portCRITICAL_NESTING_IN_TCB     0
#define portCLEAN_UP_TCB(pxTCB)         /* no-op */
#define portTASK_FUNCTION_PROTO(fn, params)  void fn(void *params)
#define portTASK_FUNCTION(fn, params)        void fn(void *params)
#define portCONFIGURE_TIMER_FOR_RUN_TIME_STATS()
#define portGET_RUN_TIME_COUNTER_VALUE()     0

/* ── Trace macros — used by intr_alloc.c (shared_intr_isr) ──────── */
#ifndef traceISR_ENTER
#define traceISR_ENTER(_n_)
#endif
#ifndef traceISR_EXIT
#define traceISR_EXIT()
#endif
#ifndef traceISR_EXIT_TO_SCHEDULER
#define traceISR_EXIT_TO_SCHEDULER()
#endif

/* ── os_task_switch_is_pended — used by intr_alloc.c ────────────── */
#define os_task_switch_is_pended(_cpu_)  (false)

/* Legacy type aliases — must match real portmacro.h definitions.
 * portBASE_TYPE is used by i2c_slave.c, i2c_master.c, jpeg drivers. */
#define portBASE_TYPE   int
#define portSTACK_TYPE  uint8_t
#define portCHAR        uint8_t
#define portFLOAT       float
#define portDOUBLE      double
#define portLONG        int32_t
#define portSHORT       int16_t

#endif /* PORTMACRO_H */
