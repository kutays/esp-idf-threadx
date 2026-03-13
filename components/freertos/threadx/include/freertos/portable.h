// SPDX-License-Identifier: Apache-2.0
/*
 * portable.h — FreeRTOS portable layer stub for ThreadX compat
 *
 * In real FreeRTOS, this provides pvPortMalloc/vPortFree and port-specific
 * functions. For ThreadX compat, memory allocation goes through tx_byte_allocate.
 * We provide the declarations here for ESP-IDF code that includes this header.
 */
#ifndef FREERTOS_PORTABLE_H
#define FREERTOS_PORTABLE_H

#include "FreeRTOS.h"
#include "freertos/portmacro.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Memory allocation — implemented in port.c using ThreadX byte pool */
void *pvPortMalloc(size_t xWantedSize);
void vPortFree(void *pv);
size_t xPortGetFreeHeapSize(void);
size_t xPortGetMinimumEverFreeHeapSize(void);

/* Port-specific initialization (stub) */
BaseType_t xPortStartScheduler(void);
void vPortEndScheduler(void);

#ifdef __cplusplus
}
#endif

#endif /* FREERTOS_PORTABLE_H */
