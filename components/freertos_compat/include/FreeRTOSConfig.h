/*
 * FreeRTOSConfig.h — Configuration for ThreadX FreeRTOS compatibility layer
 *
 * Consumed by the upstream tx_freertos.c / FreeRTOS.h.
 * MUST NOT include ESP-IDF's real FreeRTOS headers (portmacro.h etc.)
 * to avoid type and macro conflicts.
 */

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <stdint.h>

/* Core configuration */
#define configTICK_RATE_HZ                      (100u)
#define configMAX_PRIORITIES                    (32u)
#define configMINIMAL_STACK_SIZE                (512u)
#define configTOTAL_HEAP_SIZE                   (1024u * 32u)
#define configUSE_16_BIT_TICKS                  0
#define configSTACK_DEPTH_TYPE                  uint32_t
#define configENABLE_BACKWARD_COMPATIBILITY     1

#define INCLUDE_vTaskDelete                     1

/* Use auto-init so the compat layer creates its byte pool automatically */
#define TX_FREERTOS_AUTO_INIT                   1

/* Assertion macros */
#define configASSERT(x)
#define TX_FREERTOS_ASSERT_FAIL()

/* Allocation support */
#define portUSING_MPU_WRAPPERS                  0
#define configSUPPORT_DYNAMIC_ALLOCATION        1
#define configSUPPORT_STATIC_ALLOCATION         1

/*
 * Port macros — defined here directly to avoid pulling in ESP-IDF's
 * real portmacro.h which conflicts with the ThreadX compat layer.
 * The upstream FreeRTOS.h checks #ifndef for most of these.
 */
extern void vPortYield(void);
#define portYIELD()                         vPortYield()
#define portYIELD_FROM_ISR(x)               do { if(x) vPortYield(); } while(0)
#define portEND_SWITCHING_ISR(x)            portYIELD_FROM_ISR(x)

/* Interrupt control for RISC-V */
#define portDISABLE_INTERRUPTS()            do { __asm__ volatile("csrc mstatus, 8"); } while(0)
#define portENABLE_INTERRUPTS()             do { __asm__ volatile("csrs mstatus, 8"); } while(0)

/* ISR critical sections */
#define taskENTER_CRITICAL_FROM_ISR()       ({ unsigned long __rv; __asm__ volatile("csrrc %0, mstatus, 8" : "=r"(__rv)); __rv; })
#define taskEXIT_CRITICAL_FROM_ISR(x)       do { if((x) & 0x8) { __asm__ volatile("csrs mstatus, 8"); } } while(0)

/* ESP-IDF specific */
#define portNUM_PROCESSORS                  1
#define xPortGetCoreID()                    0

#endif /* FREERTOS_CONFIG_H */
