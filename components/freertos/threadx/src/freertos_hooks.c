// SPDX-License-Identifier: Apache-2.0
/*
 * freertos_hooks.c — Stub implementations of FreeRTOS hooks
 *
 * Some ESP-IDF components call these directly (not through the hooks API).
 * Under ThreadX these are no-ops.
 */

#include <stdbool.h>

/* Idle hook — called from idle task (if it existed) */
bool vApplicationIdleHook(void) { return true; }

/* Tick hook — called from tick ISR */
void vApplicationTickHook(void) { }

/* Stack overflow hook */
void vApplicationStackOverflowHook(void *xTask, char *pcTaskName)
{
    (void)xTask;
    (void)pcTaskName;
}
