/*
 * freertos_debug.h — Stub for ESP-IDF debugging features (esp_gdbstub, panic handler)
 *
 * The real ESP-IDF freertos_debug.h provides task snapshot/iteration APIs used by
 * the panic handler and GDB stub. On ThreadX, we provide the type definitions so
 * code compiles, and stub implementations that return empty results.
 *
 * ListItem_t: Real FreeRTOS uses this in TaskIterator_t for linked-list traversal.
 * We typedef it to void since ThreadX doesn't use FreeRTOS lists — the iterator
 * stubs return -1 (no tasks) immediately.
 */

#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/list.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Task Snapshot ────────────────────────────────────────────────── */

typedef struct xTASK_SNAPSHOT {
    void *pxTCB;
    StackType_t *pxTopOfStack;
    StackType_t *pxEndOfStack;
} TaskSnapshot_t;

typedef struct TaskIterator {
    UBaseType_t uxCurrentListIndex;
    ListItem_t *pxNextListItem;
    TaskHandle_t pxTaskHandle;
} TaskIterator_t;

int xTaskGetNext(TaskIterator_t *xIterator);
BaseType_t vTaskGetSnapshot(TaskHandle_t pxTask, TaskSnapshot_t *pxTaskSnapshot);
UBaseType_t uxTaskGetSnapshotAll(TaskSnapshot_t * const pxTaskSnapshotArray,
                                  const UBaseType_t uxArrayLength,
                                  UBaseType_t * const pxTCBSize);

/* pvTaskGetCurrentTCBForCore — already declared in FreeRTOS.h, but
 * some callers include this header directly. */
void *pvTaskGetCurrentTCBForCore(BaseType_t xCoreID);

#ifdef __cplusplus
}
#endif
