/*
 * tx_user.h — ThreadX configuration overrides for ESP32-C6
 *
 * This file is included by ThreadX when TX_INCLUDE_USER_DEFINE_FILE is defined.
 * It allows us to override default ThreadX configuration without modifying
 * upstream source files.
 */

#ifndef TX_USER_H
#define TX_USER_H

/* Tick rate from menuconfig (default 100 Hz) */
#define TX_TIMER_TICKS_PER_SECOND       CONFIG_THREADX_TICK_RATE_HZ

/* Maximum priority levels (0 = highest priority) */
#define TX_MAX_PRIORITIES               CONFIG_THREADX_MAX_PRIORITIES

/* Enable ThreadX event trace (disable for production to save RAM) */
/* #define TX_ENABLE_EVENT_TRACE */

/* Enable stack checking for debug builds */
#define TX_ENABLE_STACK_CHECKING

/* No timer processing in ISR — use the internal timer thread */
/* #define TX_NO_TIMER */

/* Inline thread suspend/resume for performance */
/* #define TX_INLINE_THREAD_RESUME_SUSPEND */

/* Use preset global C data (faster startup, ThreadX zeroes its own globals) */
/* #define TX_NO_GLOBAL_VARIABLE_INIT */

/* Don't use MTIME/MTIMECMP — we use ESP32-C6 SYSTIMER instead */
#define TX_PORT_USE_CUSTOM_TIMER        1

/* Redefine timer setup to call our SYSTIMER init (called from low-level init) */
#define TX_PORT_SPECIFIC_PRE_SCHEDULER_INITIALIZATION

/* Required by the ThreadX FreeRTOS compatibility layer (tx_freertos.c).
 * Adds a back-pointer from TX_THREAD to the FreeRTOS task wrapper struct. */
#define TX_THREAD_USER_EXTENSION        VOID *txfr_thread_ptr;

#endif /* TX_USER_H */
