// SPDX-License-Identifier: Apache-2.0
/*
 * FreeRTOS.h — Wrapper that includes the upstream ThreadX compat layer's
 * FreeRTOS.h (all type definitions and function declarations) then adds
 * ESP-IDF-specific extensions.
 *
 * Include order:
 *   1. This file (found first because threadx/include/ is first in path)
 *   2. #include_next finds upstream compat FreeRTOS.h in TXFR_DIR
 *   3. Upstream includes FreeRTOSConfig.h (our version, with port macros)
 *   4. We undef/redefine macros that need ESP-IDF signatures
 */

#ifndef FREERTOS_WRAPPER_H
#define FREERTOS_WRAPPER_H

/* Include the upstream ThreadX compat layer FreeRTOS.h.
 * This provides all type definitions (txfr_task_t, txfr_queue_t, etc.)
 * and function declarations (xTaskCreate, xQueueSend, etc.). */
#include_next <FreeRTOS.h>

/* ── Override upstream compat macros with ESP-IDF signatures ─────
 * The upstream compat FreeRTOS.h defines port macros with generic signatures.
 * We #undef every macro that BOTH upstream AND our portmacro.h define, so
 * portmacro.h can redefine them with ESP32-C6 RISC-V implementations.
 *
 * IMPORTANT: Only undef macros that portmacro.h actually redefines!
 * Macros like taskYIELD, taskDISABLE_INTERRUPTS, taskENTER_CRITICAL_FROM_ISR
 * are defined by upstream only — do NOT undef them or they'll vanish.
 *
 * Complete list of conflicts (upstream line → portmacro.h line):
 *   portCRITICAL_NESTING_IN_TCB    (40 → 162)
 *   portCLEAN_UP_TCB               (41 → 163)
 *   portCONFIGURE_TIMER_FOR_RUN_TIME_STATS (44 → 166)
 *   portYIELD_WITHIN_API           (46 → 156)
 *   portASSERT_IF_IN_ISR           (54 → 155)
 *   portTICK_TYPE_IS_ATOMIC        (55 → 161)
 *   portENTER_CRITICAL             (321 → 85)
 *   portEXIT_CRITICAL              (322 → 86)
 *   taskENTER_CRITICAL             (319 → 93)
 *   taskEXIT_CRITICAL              (320 → 94)
 */

/* Critical sections — upstream uses no-arg, ESP-IDF uses spinlock arg */
#undef portENTER_CRITICAL
#undef portEXIT_CRITICAL
#undef taskENTER_CRITICAL
#undef taskEXIT_CRITICAL

/* Port utility macros — upstream defines generic versions, portmacro.h
 * redefines with ESP32-C6 specific implementations */
#undef portCRITICAL_NESTING_IN_TCB
#undef portCLEAN_UP_TCB
#undef portCONFIGURE_TIMER_FOR_RUN_TIME_STATS
#undef portYIELD_WITHIN_API
#undef portASSERT_IF_IN_ISR
#undef portTICK_TYPE_IS_ATOMIC

#include "freertos/portmacro.h"

/* Transitive include: ESP-IDF's real FreeRTOS.h pulls in esp_system.h through
 * the config header chain. esp_adapter.c uses esp_get_free_internal_heap_size()
 * without including esp_system.h directly — it relies on this transitive path. */
#include "esp_system.h"

/* ── Static type stubs ───────────────────────────────────────────
 * FreeRTOS defines these for static allocation of internal structures.
 * esp_ringbuf's ringbuf.c has a _Static_assert checking
 *   sizeof(StaticRingbuffer_t) == sizeof(Ringbuffer_t)
 * where Ringbuffer_t contains List_t and StaticRingbuffer_t contains
 * StaticList_t. So these MUST be size-identical to our list.h types.
 *
 * Our list.h defines MiniListItem_t = ListItem_t (no mini optimization),
 * so both have 5 fields × 4 bytes = 20 bytes on RISC-V 32-bit.
 *
 * Size verification:
 *   ListItem_t       = {xItemValue, pxNext, pxPrev, pvOwner, pxContainer} = 20
 *   MiniListItem_t   = ListItem_t = 20
 *   List_t           = {uxNumberOfItems(4) + pxIndex(4) + xListEnd(20)} = 28
 *   StaticListItem_t = {xDummy1(4) + xDummy2[4](16)} = 20 ✓
 *   StaticMiniListItem_t = same = 20 ✓
 *   StaticList_t     = {uxDummy1(4) + pvDummy2(4) + xDummy3(20)} = 28 ✓
 */
struct xSTATIC_LIST_ITEM {
    TickType_t xDummy1;
    void *xDummy2[4];
};
typedef struct xSTATIC_LIST_ITEM StaticListItem_t;

struct xSTATIC_MINI_LIST_ITEM {
    TickType_t xDummy1;
    void *xDummy2[4];
};
typedef struct xSTATIC_MINI_LIST_ITEM StaticMiniListItem_t;

typedef struct xSTATIC_LIST {
    UBaseType_t uxDummy1;
    void *pvDummy2;
    StaticMiniListItem_t xDummy3;
} StaticList_t;

/* ════════════════════════════════════════════════════════════════════
 * Internal FreeRTOS types and functions
 *
 * Real FreeRTOS defines these in task.h/queue.h. The upstream compat layer
 * provides the public API (xSemaphoreTake, xQueueCreate, etc.) but not the
 * internal functions that ESP-IDF code sometimes calls directly.
 * ════════════════════════════════════════════════════════════════════ */

/* ── TimeOut_t — timeout state for polling loops ───────────────────
 * Used by vTaskSetTimeOutState/xTaskCheckForTimeOut in drivers like
 * usb_serial_jtag, esp_netif, etc. for non-blocking wait patterns. */
typedef struct xTIME_OUT {
    BaseType_t xOverflowCount;
    TickType_t xTimeOnEntering;
} TimeOut_t;

void vTaskSetTimeOutState(TimeOut_t * const pxTimeOut);
void vTaskInternalSetTimeOutState(TimeOut_t * const pxTimeOut);
BaseType_t xTaskCheckForTimeOut(TimeOut_t * const pxTimeOut,
                                 TickType_t * const pxTicksToWait);

/* ── xQueueSemaphoreTake — internal queue function ─────────────────
 * In real FreeRTOS, xSemaphoreTake is a macro → xQueueSemaphoreTake().
 * ESP-IDF code (esp_netif_sntp.c) calls xQueueSemaphoreTake() directly. */
#define xQueueSemaphoreTake(xQueue, xTicksToWait)  xSemaphoreTake((xQueue), (xTicksToWait))

/* ── xQueueGenericCreate — internal queue creation ─────────────────
 * In real FreeRTOS, xQueueCreate is a macro → xQueueGenericCreate().
 * Some ESP-IDF code calls xQueueGenericCreate() directly.
 * queueQUEUE_TYPE_BASE = 0 (standard queue). */
/* ── Queue/semaphore type constants ────────────────────────────────
 * Used by internal FreeRTOS functions (xQueueCreateMutex, etc.) and
 * ESP-IDF's idf_additions (WithCaps wrappers). Defined early so inline
 * functions below can reference them. Guarded to avoid redefinition if
 * real FreeRTOS queue.h is ever transitively included. */
#ifndef queueQUEUE_TYPE_BASE
#define queueQUEUE_TYPE_BASE               0
#define queueQUEUE_TYPE_MUTEX              1
#define queueQUEUE_TYPE_COUNTING_SEMAPHORE 2
#define queueQUEUE_TYPE_BINARY_SEMAPHORE   3
#define queueQUEUE_TYPE_RECURSIVE_MUTEX    4
#endif

#define xQueueGenericCreate(uxQueueLength, uxItemSize, ucQueueType) \
    xQueueCreate((uxQueueLength), (uxItemSize))

/* ── xQueueCreateMutex — internal mutex creation ──────────────────
 * In real FreeRTOS, xSemaphoreCreateMutex() is a macro → xQueueCreateMutex().
 * newlib/locks.c calls xQueueCreateMutex() directly. Dispatch based on type. */
static inline SemaphoreHandle_t xQueueCreateMutex(const uint8_t ucQueueType) {
    if (ucQueueType == queueQUEUE_TYPE_RECURSIVE_MUTEX) {
        return xSemaphoreCreateRecursiveMutex();
    }
    return xSemaphoreCreateMutex();
}

/* ── xQueueCreateMutexStatic — internal static mutex creation ──── */
static inline SemaphoreHandle_t xQueueCreateMutexStatic(const uint8_t ucQueueType,
                                                          StaticSemaphore_t *pxStaticBuf) {
    if (ucQueueType == queueQUEUE_TYPE_RECURSIVE_MUTEX) {
        return xSemaphoreCreateRecursiveMutexStatic(pxStaticBuf);
    }
    return xSemaphoreCreateMutexStatic(pxStaticBuf);
}

/* ── xQueueGenericSend — used by esp_adapter.c ──────────────────── */
BaseType_t xQueueGenericSend(QueueHandle_t xQueue, const void *pvItemToQueue,
                             TickType_t xTicksToWait, BaseType_t xCopyPosition);

/* ── Thread-local storage — needed by pthread (WiFi adapter) ───── */
void vTaskSetThreadLocalStoragePointer(TaskHandle_t xTaskToSet,
                                       BaseType_t xIndex, void *pvValue);
void *pvTaskGetThreadLocalStoragePointer(TaskHandle_t xTaskToQuery,
                                         BaseType_t xIndex);

typedef void (*TlsDeleteCallbackFunction_t)(int, void *);
void vTaskSetThreadLocalStoragePointerAndDelCallback(
    TaskHandle_t xTaskToSet, BaseType_t xIndex, void *pvValue,
    TlsDeleteCallbackFunction_t pvDelCallback);

/* ── xTaskCreatePinnedToCore — ESP-IDF specific ─────────────────── */
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t pvTaskCode,
                                    const char * const pcName,
                                    const configSTACK_DEPTH_TYPE usStackDepth,
                                    void * const pvParameters,
                                    UBaseType_t uxPriority,
                                    TaskHandle_t * const pxCreatedTask,
                                    const BaseType_t xCoreID);

/* ── ISR context detection ───────────────────────────────────────── */
BaseType_t xPortInIsrContext(void);

/* ── uxTaskGetStackHighWaterMark — may be needed by some components */
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t xTask);

/* ════════════════════════════════════════════════════════════════════
 * ESP-IDF FreeRTOS Additions (idf_additions.c / freertos_compatibility.c)
 *
 * ESP-IDF extends FreeRTOS with ~50 functions. When we override the freertos
 * component, idf_additions.c is no longer compiled, so we must provide all
 * functions that other ESP-IDF components call. Implementations are in port.c.
 *
 * Functions from upstream tx_freertos.c that ESP-IDF also defines (like
 * vTaskDelayUntil, ulTaskNotifyTake, xTaskNotifyWait) are already provided
 * by the upstream compat layer and are NOT re-declared here.
 * ════════════════════════════════════════════════════════════════════ */

/* ── Task creation with memory caps ─────────────────────────────── */
BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t pvTaskCode,
                                            const char * const pcName,
                                            const configSTACK_DEPTH_TYPE usStackDepth,
                                            void * const pvParameters,
                                            UBaseType_t uxPriority,
                                            TaskHandle_t * const pxCreatedTask,
                                            const BaseType_t xCoreID,
                                            UBaseType_t uxMemoryCaps);
void vTaskDeleteWithCaps(TaskHandle_t xTaskToDelete);

static inline BaseType_t xTaskCreateWithCaps(TaskFunction_t pvTaskCode,
                                              const char * const pcName,
                                              configSTACK_DEPTH_TYPE usStackDepth,
                                              void * const pvParameters,
                                              UBaseType_t uxPriority,
                                              TaskHandle_t * pvCreatedTask,
                                              UBaseType_t uxMemoryCaps) {
    return xTaskCreatePinnedToCoreWithCaps(pvTaskCode, pcName, usStackDepth,
                                            pvParameters, uxPriority, pvCreatedTask,
                                            tskNO_AFFINITY, uxMemoryCaps);
}

/* ── Queue creation with memory caps ────────────────────────────── */
QueueHandle_t xQueueCreateWithCaps(UBaseType_t uxQueueLength,
                                    UBaseType_t uxItemSize,
                                    UBaseType_t uxMemoryCaps);
void vQueueDeleteWithCaps(QueueHandle_t xQueue);

/* ── Semaphore creation with memory caps ────────────────────────── */
SemaphoreHandle_t xSemaphoreCreateGenericWithCaps(UBaseType_t uxMaxCount,
                                                   UBaseType_t uxInitialCount,
                                                   const uint8_t ucQueueType,
                                                   UBaseType_t uxMemoryCaps);
void vSemaphoreDeleteWithCaps(SemaphoreHandle_t xSemaphore);

/* Convenience inline wrappers matching ESP-IDF's idf_additions.h */
static inline SemaphoreHandle_t xSemaphoreCreateBinaryWithCaps(UBaseType_t uxMemoryCaps) {
    return xSemaphoreCreateGenericWithCaps(1, 0, queueQUEUE_TYPE_BINARY_SEMAPHORE, uxMemoryCaps);
}
static inline SemaphoreHandle_t xSemaphoreCreateCountingWithCaps(UBaseType_t uxMaxCount,
                                                                   UBaseType_t uxInitialCount,
                                                                   UBaseType_t uxMemoryCaps) {
    return xSemaphoreCreateGenericWithCaps(uxMaxCount, uxInitialCount, queueQUEUE_TYPE_COUNTING_SEMAPHORE, uxMemoryCaps);
}
static inline SemaphoreHandle_t xSemaphoreCreateMutexWithCaps(UBaseType_t uxMemoryCaps) {
    return xSemaphoreCreateGenericWithCaps(1, 1, queueQUEUE_TYPE_MUTEX, uxMemoryCaps);
}
static inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutexWithCaps(UBaseType_t uxMemoryCaps) {
    return xSemaphoreCreateGenericWithCaps(1, 1, queueQUEUE_TYPE_RECURSIVE_MUTEX, uxMemoryCaps);
}

/* ── Event group creation with memory caps ──────────────────────── */
EventGroupHandle_t xEventGroupCreateWithCaps(UBaseType_t uxMemoryCaps);
void vEventGroupDeleteWithCaps(EventGroupHandle_t xEventGroup);

/* ── Stream/message buffer with memory caps (stubs) ─────────────── */
/* Forward-declare StreamBufferHandle_t — not in upstream compat FreeRTOS.h
 * because stream buffers aren't part of the ThreadX compat layer. */
typedef void *StreamBufferHandle_t;
typedef void *MessageBufferHandle_t;

StreamBufferHandle_t xStreamBufferGenericCreateWithCaps(size_t xBufferSizeBytes,
                                                         size_t xTriggerLevelBytes,
                                                         BaseType_t xIsMessageBuffer,
                                                         UBaseType_t uxMemoryCaps);
void vStreamBufferGenericDeleteWithCaps(StreamBufferHandle_t xStreamBuffer,
                                         BaseType_t xIsMessageBuffer);

static inline StreamBufferHandle_t xStreamBufferCreateWithCaps(size_t xBufferSizeBytes,
                                                                 size_t xTriggerLevelBytes,
                                                                 UBaseType_t uxMemoryCaps) {
    return xStreamBufferGenericCreateWithCaps(xBufferSizeBytes, xTriggerLevelBytes, pdFALSE, uxMemoryCaps);
}
static inline void vStreamBufferDeleteWithCaps(StreamBufferHandle_t xStreamBuffer) {
    vStreamBufferGenericDeleteWithCaps(xStreamBuffer, pdFALSE);
}

/* ── Legacy compatibility (freertos_compatibility.c) ────────────── */
BaseType_t xQueueGenericReceive(QueueHandle_t xQueue, void * const pvBuffer,
                                 TickType_t xTicksToWait, const BaseType_t xPeek);

/* ── Task utilities ─────────────────────────────────────────────── */
BaseType_t xTaskGetCoreID(TaskHandle_t xTask);
TaskHandle_t xTaskGetIdleTaskHandleForCore(BaseType_t xCoreID);
TaskHandle_t xTaskGetCurrentTaskHandleForCore(BaseType_t xCoreID);
void *pvTaskGetCurrentTCBForCore(BaseType_t xCoreID);
uint8_t *pxTaskGetStackStart(TaskHandle_t xTask);

/* ── Heap management ────────────────────────────────────────────── */
size_t xPortGetFreeHeapSize(void);
size_t xPortGetMinimumEverFreeHeapSize(void);
bool xPortCheckValidListMem(const void *ptr);
bool xPortCheckValidTCBMem(const void *ptr);
bool xPortcheckValidStackMem(const void *ptr);

/* NOTE: Do NOT define ESP_TASK_MAIN_STACK, ESP_TASK_MAIN_PRIO, ESP_TASK_MAIN_CORE
 * here. These are authoritatively defined by esp_task.h (from esp_system component)
 * which includes this header. Defining them here causes redefinition warnings. */

#endif /* FREERTOS_WRAPPER_H */
