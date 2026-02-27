/*
 * FreeRTOSConfig.h — Configuration for ThreadX FreeRTOS compatibility layer
 *
 * Consumed by the upstream tx_freertos.c / FreeRTOS.h.
 * Defines port macros BEFORE the upstream header checks #ifndef guards,
 * so our RISC-V definitions take priority over upstream defaults.
 */

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <stdint.h>

/* Core configuration */
#define configTICK_RATE_HZ                      (100u)
#define configMAX_PRIORITIES                    (32u)
#define configMINIMAL_STACK_SIZE                (512u)
#define configTOTAL_HEAP_SIZE                   (1024u * 64u)
#define configUSE_16_BIT_TICKS                  0
#define configSTACK_DEPTH_TYPE                  uint32_t
#define configENABLE_BACKWARD_COMPATIBILITY     1

#define INCLUDE_vTaskDelete                     1

/* Disable auto-init. The auto-init constructor calls tx_freertos_init()
 * which uses ThreadX APIs (tx_byte_pool_create, tx_thread_create) —
 * these crash if called before tx_kernel_enter() initializes the kernel.
 * We call tx_freertos_init() ourselves from tx_application_define()
 * (inside tx_kernel_enter, after kernel init). Setting this to 0 also
 * disables the empty tx_application_define() in tx_freertos.c, so our
 * real one in tx_port_startup.c wins. */
#define TX_FREERTOS_AUTO_INIT                   0

/* Assertion macros */
#define configASSERT(x)
#define TX_FREERTOS_ASSERT_FAIL()

/* Allocation support */
#define portUSING_MPU_WRAPPERS                  0
#define configSUPPORT_DYNAMIC_ALLOCATION        1
#define configSUPPORT_STATIC_ALLOCATION         1

/* Thread-local storage — needed by pthread (WiFi adapter uses this) */
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS 2

/* Feature flags — checked by ESP-IDF components in #if guards */
#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             1
#define configUSE_COUNTING_SEMAPHORES           1
#define configUSE_TASK_NOTIFICATIONS            1
#define configUSE_TRACE_FACILITY                0
#define configGENERATE_RUN_TIME_STATS           0
#define configUSE_QUEUE_SETS                    0
#define configUSE_TIMERS                        1
#define configTIMER_TASK_PRIORITY               1
#define configTIMER_QUEUE_LENGTH                10
#define configTIMER_TASK_STACK_DEPTH            2048
#define configRUN_TIME_COUNTER_TYPE             uint32_t

/* INCLUDE flags — enable optional FreeRTOS functions */
#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_vTaskDelayUntil                 1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_xTaskGetCurrentTaskHandle       1
#define INCLUDE_uxTaskGetStackHighWaterMark     1
#define INCLUDE_xTaskGetHandle                  1
#define INCLUDE_eTaskGetState                   1
#define INCLUDE_xTaskAbortDelay                 1
#define INCLUDE_xSemaphoreGetMutexHolder        1

/*
 * Port macros — defined here directly so the upstream FreeRTOS.h sees them
 * before its own #ifndef guards. This avoids pulling in ESP-IDF's real
 * portmacro.h which conflicts with the ThreadX compat layer.
 */
extern void vPortYield(void);
#define portYIELD()                         vPortYield()

/* Variadic: supports both portYIELD_FROM_ISR() and portYIELD_FROM_ISR(x)
 * Bug 35: Must be no-op — vPortYield() calls tx_thread_relinquish() which is
 * a thread-level API. Calling it from ISR context corrupts ThreadX scheduler.
 * ThreadX handles preemption automatically via _tx_thread_context_restore. */
#define portYIELD_FROM_ISR(...)             ((void)0)
#define portEND_SWITCHING_ISR(...)          ((void)0)

/* Interrupt control for RISC-V: mstatus bit 3 = MIE */
#define portDISABLE_INTERRUPTS()            do { __asm__ volatile("csrc mstatus, 8"); } while(0)
#define portENABLE_INTERRUPTS()             do { __asm__ volatile("csrs mstatus, 8"); } while(0)

/* ISR critical sections */
#define taskENTER_CRITICAL_FROM_ISR()       ({ unsigned long __rv; __asm__ volatile("csrrc %0, mstatus, 8" : "=r"(__rv)); __rv; })
#define taskEXIT_CRITICAL_FROM_ISR(x)       do { if((x) & 0x8) { __asm__ volatile("csrs mstatus, 8"); } } while(0)

/* ESP-IDF specific */
#define portNUM_PROCESSORS                  1
#define xPortGetCoreID()                    0

#endif /* FREERTOS_CONFIG_H */
