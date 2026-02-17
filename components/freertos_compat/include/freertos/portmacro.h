/*
 * portmacro.h — FreeRTOS port macros for ESP32-C6 / ThreadX
 *
 * Provides the port-level macros that ESP-IDF components may include directly.
 * Most actual functionality is handled by the upstream ThreadX FreeRTOS
 * compatibility layer; this just fills in the RISC-V specific bits.
 */

#ifndef PORTMACRO_H
#define PORTMACRO_H

#include <stdint.h>

/* Type definitions */
#define portSTACK_TYPE          uint32_t
#define portBASE_TYPE           int32_t
#define portUBASE_TYPE          uint32_t
typedef portSTACK_TYPE  StackType_t;

/* Tick conversion */
#define portTICK_PERIOD_MS      (1000 / 100)  /* configTICK_RATE_HZ = 100 */

/* Stack */
#define portBYTE_ALIGNMENT      16
#define portSTACK_GROWTH        (-1)

/* Yield */
extern void vPortYield(void);
#define portYIELD()                 vPortYield()
#define portYIELD_FROM_ISR(x)       do { if(x) vPortYield(); } while(0)
#define portEND_SWITCHING_ISR(x)    portYIELD_FROM_ISR(x)

/* NOP / memory barrier */
#define portNOP()                   __asm__ volatile("nop")
#define portMEMORY_BARRIER()        __asm__ volatile("fence" ::: "memory")

/* ESP-IDF specific: single core */
#define portNUM_PROCESSORS          1
#define xPortGetCoreID()            0

#endif /* PORTMACRO_H */
