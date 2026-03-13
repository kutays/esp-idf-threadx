// SPDX-License-Identifier: Apache-2.0
/*
 * atomic.h — FreeRTOS atomic operations stub for ThreadX compat layer
 *
 * Provides atomic load/store/compare-and-swap operations.
 * On single-core ESP32-C6, these degenerate to simple operations
 * with interrupt disable/enable wrappers.
 */
#ifndef FREERTOS_ATOMIC_H
#define FREERTOS_ATOMIC_H

#include "FreeRTOS.h"

#ifndef ATOMIC_ENTER_CRITICAL
#define ATOMIC_ENTER_CRITICAL()   portDISABLE_INTERRUPTS()
#endif

#ifndef ATOMIC_EXIT_CRITICAL
#define ATOMIC_EXIT_CRITICAL()    portENABLE_INTERRUPTS()
#endif

static inline uint32_t Atomic_CompareAndSwap_u32(uint32_t volatile *pulDestination,
                                                   uint32_t ulExchange,
                                                   uint32_t ulComparand)
{
    uint32_t ulReturnValue;
    ATOMIC_ENTER_CRITICAL();
    if (*pulDestination == ulComparand) {
        *pulDestination = ulExchange;
        ulReturnValue = 1;
    } else {
        ulReturnValue = 0;
    }
    ATOMIC_EXIT_CRITICAL();
    return ulReturnValue;
}

static inline uint32_t Atomic_SwapPointers_p32(void * volatile *ppvDestination,
                                                 void *pvExchange)
{
    void *pReturnValue;
    ATOMIC_ENTER_CRITICAL();
    pReturnValue = *ppvDestination;
    *ppvDestination = pvExchange;
    ATOMIC_EXIT_CRITICAL();
    return (uint32_t)pReturnValue;
}

static inline uint32_t Atomic_CompareAndSwapPointers_p32(void * volatile *ppvDestination,
                                                           void *pvExchange,
                                                           void *pvComparand)
{
    uint32_t ulReturnValue;
    ATOMIC_ENTER_CRITICAL();
    if (*ppvDestination == pvComparand) {
        *ppvDestination = pvExchange;
        ulReturnValue = 1;
    } else {
        ulReturnValue = 0;
    }
    ATOMIC_EXIT_CRITICAL();
    return ulReturnValue;
}

static inline uint32_t Atomic_Add_u32(uint32_t volatile *pulAddend, uint32_t ulCount)
{
    uint32_t ulReturn;
    ATOMIC_ENTER_CRITICAL();
    ulReturn = *pulAddend;
    *pulAddend += ulCount;
    ATOMIC_EXIT_CRITICAL();
    return ulReturn;
}

static inline uint32_t Atomic_Subtract_u32(uint32_t volatile *pulAddend, uint32_t ulCount)
{
    uint32_t ulReturn;
    ATOMIC_ENTER_CRITICAL();
    ulReturn = *pulAddend;
    *pulAddend -= ulCount;
    ATOMIC_EXIT_CRITICAL();
    return ulReturn;
}

static inline uint32_t Atomic_Increment_u32(uint32_t volatile *pulAddend)
{
    return Atomic_Add_u32(pulAddend, 1);
}

static inline uint32_t Atomic_Decrement_u32(uint32_t volatile *pulAddend)
{
    return Atomic_Subtract_u32(pulAddend, 1);
}

static inline uint32_t Atomic_OR_u32(uint32_t volatile *pulDestination, uint32_t ulValue)
{
    uint32_t ulReturn;
    ATOMIC_ENTER_CRITICAL();
    ulReturn = *pulDestination;
    *pulDestination |= ulValue;
    ATOMIC_EXIT_CRITICAL();
    return ulReturn;
}

static inline uint32_t Atomic_AND_u32(uint32_t volatile *pulDestination, uint32_t ulValue)
{
    uint32_t ulReturn;
    ATOMIC_ENTER_CRITICAL();
    ulReturn = *pulDestination;
    *pulDestination &= ulValue;
    ATOMIC_EXIT_CRITICAL();
    return ulReturn;
}

static inline uint32_t Atomic_NAND_u32(uint32_t volatile *pulDestination, uint32_t ulValue)
{
    uint32_t ulReturn;
    ATOMIC_ENTER_CRITICAL();
    ulReturn = *pulDestination;
    *pulDestination = ~(*pulDestination & ulValue);
    ATOMIC_EXIT_CRITICAL();
    return ulReturn;
}

static inline uint32_t Atomic_XOR_u32(uint32_t volatile *pulDestination, uint32_t ulValue)
{
    uint32_t ulReturn;
    ATOMIC_ENTER_CRITICAL();
    ulReturn = *pulDestination;
    *pulDestination ^= ulValue;
    ATOMIC_EXIT_CRITICAL();
    return ulReturn;
}

#endif /* FREERTOS_ATOMIC_H */
