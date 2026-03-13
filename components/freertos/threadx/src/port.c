// SPDX-License-Identifier: Apache-2.0
/*
 * port.c — ESP-IDF-specific supplements for the ThreadX FreeRTOS compat layer
 *
 * Provides:
 *   - vPortYield (used by portYIELD macro)
 *   - vPortEnterCritical / vPortExitCritical (critical sections)
 *   - xPortInIsrContext (ISR context detection via ThreadX system state)
 *   - xQueueGenericSend (dispatches to front/back/overwrite)
 *   - Thread-local storage (pvTaskGet/vTaskSetThreadLocalStoragePointer)
 *   - xTaskCreatePinnedToCore (forwards to xTaskCreate on single-core)
 *   - Newlib retarget lock overrides (ThreadX mutexes)
 */

#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <sys/reent.h>
#include <tx_api.h>
#include "freertos/FreeRTOS.h"
#include "freertos/list.h"

/* ThreadX internal: non-zero when in ISR or initialization context */
extern volatile ULONG _tx_thread_system_state;

/* ISR stack switching state (defined in rtos_int_hooks.S) */
extern volatile uint32_t port_xSchedulerRunning;

/* PLIC MX threshold register (direct address for ESP32-C6) */
#define PLIC_MX_THRESH_REG  (*(volatile uint32_t *)(0x20001000 + 0x90))

/* ── ESP-IDF ISR initialization ─────────────────────────────────── *
 * Strong override of the weak noop in tx_port_startup.c.
 * Called from tx_application_define() before the scheduler starts.
 *
 * Sets up two things needed for ESP-IDF interrupt coexistence:
 *
 * 1. PLIC threshold = 0: ESP-IDF startup sets threshold = 1
 *    (RVHAL_INTR_ENABLE_THRESH). esp_intr_alloc() assigns priority 1
 *    by default, so all ESP-IDF interrupts (esp_timer, WiFi, BLE)
 *    are masked when threshold = 1. Setting threshold to 0 allows
 *    all interrupts with priority >= 1 to fire. Our SYSTIMER uses
 *    priority 2, so it still fires. MIE = 0 during this call (Bug 29
 *    fix), so nothing fires until the scheduler enables MIE via mret.
 *
 * 2. port_xSchedulerRunning = 1: tells rtos_int_enter/rtos_int_exit
 *    (rtos_int_hooks.S) to perform ISR stack switching. Without this,
 *    ESP-IDF interrupts would run on whatever stack happens to be
 *    active, which could overflow a ThreadX thread stack.
 */
void _tx_port_esp_idf_isr_init(void)
{
    PLIC_MX_THRESH_REG = 0;
    port_xSchedulerRunning = 1;
}

/* ── Yield ───────────────────────────────────────────────────────── */

void vPortYield(void)
{
    tx_thread_relinquish();
}

/* ── Critical sections ───────────────────────────────────────────── *
 * vPortEnterCritical / vPortExitCritical are provided by the upstream
 * tx_freertos.c compat layer. Its versions:
 *   - Disable interrupts via portDISABLE_INTERRUPTS()
 *   - Increment _tx_thread_preempt_disable (prevents ThreadX preemption)
 *   - On exit, only re-enable when nesting count reaches 0
 * This integrates correctly with both interrupt masking and ThreadX
 * preemption control. No need for our own implementation.
 */

/* ── vTaskSwitchContext ──────────────────────────────────────────── *
 * In real FreeRTOS, called from rtos_int_exit when xPortSwitchFlag is set.
 * Selects the highest-priority ready task for context switch.
 *
 * For ThreadX, preemption is handled by the ThreadX scheduler internally.
 * When tx_semaphore_put (or other TX API) resumes a higher-priority thread
 * during an ISR, _tx_thread_execute_ptr is updated. The actual context
 * switch happens at the next ThreadX timer tick (vector[17]) via
 * _tx_thread_context_save/_tx_thread_context_restore.
 *
 * This stub exists for the rtos_int_exit xPortSwitchFlag path (currently
 * dead code since portYIELD_FROM_ISR is a no-op) and to satisfy the linker.
 */
void vTaskSwitchContext(int xCoreID)
{
    (void)xCoreID;
    /* ThreadX handles context switches via _tx_thread_system_preempt_check */
}

/* ── ISR context detection ───────────────────────────────────────── */

BaseType_t xPortInIsrContext(void)
{
    /* ThreadX sets _tx_thread_system_state to non-zero during:
     *   - ISR handling (incremented on ISR entry)
     *   - System initialization (before scheduler starts)
     * When threads are running normally, it's 0. */
    return (_tx_thread_system_state != 0) ? pdTRUE : pdFALSE;
}

/* ── Newlib reentrancy ───────────────────────────────────────────── *
 * Newlib calls __getreent() to get the per-thread reentrancy struct
 * (errno, stdio buffers, etc.). In real FreeRTOS, each task has its
 * own reent struct (TCB->xTLSBlock). For ThreadX, we always return
 * the global reent struct. This is safe for single-core ESP32-C6.
 *
 * Without this, the linker falls back to the default libgloss version
 * that returns _impure_ptr which may be NULL during early boot,
 * causing vprintf to crash with "Load access fault" at address 0x8.
 */
struct _reent *__getreent(void)
{
    return _GLOBAL_REENT;
}

/* ── xQueueGenericSend ───────────────────────────────────────────── */

BaseType_t xQueueGenericSend(QueueHandle_t xQueue, const void *pvItemToQueue,
                             TickType_t xTicksToWait, BaseType_t xCopyPosition)
{
    if (xCopyPosition == queueSEND_TO_FRONT) {
        return xQueueSendToFront(xQueue, pvItemToQueue, xTicksToWait);
    } else if (xCopyPosition == queueOVERWRITE) {
        return xQueueOverwrite(xQueue, pvItemToQueue);
    } else {
        /* queueSEND_TO_BACK (default) */
        return xQueueSendToBack(xQueue, pvItemToQueue, xTicksToWait);
    }
}

/* ── Thread-Local Storage (TLS) ──────────────────────────────────
 *
 * WiFi uses pthread TLS, which calls pvTaskGetThreadLocalStoragePointer.
 * We maintain a simple table of per-task TLS arrays.
 */

#define MAX_TLS_TASKS   16
#define NUM_TLS_SLOTS   configNUM_THREAD_LOCAL_STORAGE_POINTERS

typedef struct {
    TX_THREAD *thread;
    void *values[NUM_TLS_SLOTS];
    TlsDeleteCallbackFunction_t destructors[NUM_TLS_SLOTS];
} tls_entry_t;

static tls_entry_t tls_table[MAX_TLS_TASKS];

static tls_entry_t *tls_find_or_create(TX_THREAD *thread)
{
    tls_entry_t *free_slot = NULL;

    for (int i = 0; i < MAX_TLS_TASKS; i++) {
        if (tls_table[i].thread == thread) {
            return &tls_table[i];
        }
        if (free_slot == NULL && tls_table[i].thread == NULL) {
            free_slot = &tls_table[i];
        }
    }

    if (free_slot != NULL) {
        memset(free_slot, 0, sizeof(*free_slot));
        free_slot->thread = thread;
    }
    return free_slot;
}

void vTaskSetThreadLocalStoragePointer(TaskHandle_t xTaskToSet,
                                       BaseType_t xIndex, void *pvValue)
{
    if (xIndex < 0 || xIndex >= NUM_TLS_SLOTS) return;

    TX_THREAD *thread;
    if (xTaskToSet == NULL) {
        thread = tx_thread_identify();
    } else {
        thread = &xTaskToSet->thread;
    }
    if (thread == NULL) return;

    TX_INTERRUPT_SAVE_AREA
    TX_DISABLE
    tls_entry_t *entry = tls_find_or_create(thread);
    if (entry != NULL) {
        entry->values[xIndex] = pvValue;
    }
    TX_RESTORE
}

void *pvTaskGetThreadLocalStoragePointer(TaskHandle_t xTaskToQuery,
                                         BaseType_t xIndex)
{
    if (xIndex < 0 || xIndex >= NUM_TLS_SLOTS) return NULL;

    TX_THREAD *thread;
    if (xTaskToQuery == NULL) {
        thread = tx_thread_identify();
    } else {
        thread = &xTaskToQuery->thread;
    }
    if (thread == NULL) return NULL;

    TX_INTERRUPT_SAVE_AREA
    TX_DISABLE
    void *result = NULL;
    for (int i = 0; i < MAX_TLS_TASKS; i++) {
        if (tls_table[i].thread == thread) {
            result = tls_table[i].values[xIndex];
            break;
        }
    }
    TX_RESTORE
    return result;
}

void vTaskSetThreadLocalStoragePointerAndDelCallback(
    TaskHandle_t xTaskToSet, BaseType_t xIndex, void *pvValue,
    TlsDeleteCallbackFunction_t pvDelCallback)
{
    if (xIndex < 0 || xIndex >= NUM_TLS_SLOTS) return;

    TX_THREAD *thread;
    if (xTaskToSet == NULL) {
        thread = tx_thread_identify();
    } else {
        thread = &xTaskToSet->thread;
    }
    if (thread == NULL) return;

    TX_INTERRUPT_SAVE_AREA
    TX_DISABLE
    tls_entry_t *entry = tls_find_or_create(thread);
    if (entry != NULL) {
        entry->values[xIndex] = pvValue;
        entry->destructors[xIndex] = pvDelCallback;
    }
    TX_RESTORE
}

/* ── xTaskCreatePinnedToCore ─────────────────────────────────────── */

BaseType_t xTaskCreatePinnedToCore(
    TaskFunction_t pvTaskCode,
    const char * const pcName,
    const configSTACK_DEPTH_TYPE usStackDepth,
    void * const pvParameters,
    UBaseType_t uxPriority,
    TaskHandle_t * const pxCreatedTask,
    const BaseType_t xCoreID)
{
    (void)xCoreID;  /* ESP32-C6 is single-core */
    return xTaskCreate(pvTaskCode, pcName, usStackDepth, pvParameters,
                       uxPriority, pxCreatedTask);
}

/* ── Heap-caps task API (ESP-IDF additions) ──────────────────────
 * ESP-IDF's esp_http_server and others use these for memory-capability-aware
 * task creation. On ThreadX we ignore caps and forward to standard APIs.
 */

BaseType_t xTaskCreatePinnedToCoreWithCaps(
    TaskFunction_t pvTaskCode,
    const char * const pcName,
    const configSTACK_DEPTH_TYPE usStackDepth,
    void * const pvParameters,
    UBaseType_t uxPriority,
    TaskHandle_t * const pxCreatedTask,
    const BaseType_t xCoreID,
    UBaseType_t uxMemoryCaps)
{
    (void)xCoreID;       /* ESP32-C6 is single-core */
    (void)uxMemoryCaps;  /* ThreadX doesn't distinguish memory capabilities */
    return xTaskCreate(pvTaskCode, pcName, usStackDepth, pvParameters,
                       uxPriority, pxCreatedTask);
}

void vTaskDeleteWithCaps(TaskHandle_t xTaskToDelete)
{
    vTaskDelete(xTaskToDelete);
}

/* ── Queue creation with memory caps ─────────────────────────────── */

QueueHandle_t xQueueCreateWithCaps(UBaseType_t uxQueueLength,
                                    UBaseType_t uxItemSize,
                                    UBaseType_t uxMemoryCaps)
{
    (void)uxMemoryCaps;
    return xQueueCreate(uxQueueLength, uxItemSize);
}

void vQueueDeleteWithCaps(QueueHandle_t xQueue)
{
    vQueueDelete(xQueue);
}

/* ── Semaphore creation with memory caps ─────────────────────────── */

SemaphoreHandle_t xSemaphoreCreateGenericWithCaps(UBaseType_t uxMaxCount,
                                                   UBaseType_t uxInitialCount,
                                                   const uint8_t ucQueueType,
                                                   UBaseType_t uxMemoryCaps)
{
    (void)uxMemoryCaps;
    switch (ucQueueType) {
    case 1: /* queueQUEUE_TYPE_MUTEX */
        return xSemaphoreCreateMutex();
    case 4: /* queueQUEUE_TYPE_RECURSIVE_MUTEX */
        return xSemaphoreCreateRecursiveMutex();
    case 3: /* queueQUEUE_TYPE_BINARY_SEMAPHORE */
        return xSemaphoreCreateBinary();
    default: /* queueQUEUE_TYPE_COUNTING_SEMAPHORE and others */
        return xSemaphoreCreateCounting(uxMaxCount, uxInitialCount);
    }
}

void vSemaphoreDeleteWithCaps(SemaphoreHandle_t xSemaphore)
{
    vSemaphoreDelete(xSemaphore);
}

/* ── Event group creation with memory caps ───────────────────────── */

EventGroupHandle_t xEventGroupCreateWithCaps(UBaseType_t uxMemoryCaps)
{
    (void)uxMemoryCaps;
    return xEventGroupCreate();
}

void vEventGroupDeleteWithCaps(EventGroupHandle_t xEventGroup)
{
    vEventGroupDelete(xEventGroup);
}

/* ── Stream/message buffer with memory caps (stubs) ──────────────── */

StreamBufferHandle_t xStreamBufferGenericCreateWithCaps(size_t xBufferSizeBytes,
                                                         size_t xTriggerLevelBytes,
                                                         BaseType_t xIsMessageBuffer,
                                                         UBaseType_t uxMemoryCaps)
{
    (void)xBufferSizeBytes;
    (void)xTriggerLevelBytes;
    (void)xIsMessageBuffer;
    (void)uxMemoryCaps;
    /* Stream buffers not implemented in ThreadX compat layer */
    return NULL;
}

void vStreamBufferGenericDeleteWithCaps(StreamBufferHandle_t xStreamBuffer,
                                         BaseType_t xIsMessageBuffer)
{
    (void)xStreamBuffer;
    (void)xIsMessageBuffer;
}

/* ── Legacy compatibility (freertos_compatibility.c) ─────────────── */

BaseType_t xQueueGenericReceive(QueueHandle_t xQueue, void * const pvBuffer,
                                 TickType_t xTicksToWait, const BaseType_t xPeek)
{
    if (xPeek == pdTRUE) {
        return xQueuePeek(xQueue, pvBuffer, xTicksToWait);
    } else {
        return xQueueReceive(xQueue, pvBuffer, xTicksToWait);
    }
}

/* ── Task utilities ──────────────────────────────────────────────── */

BaseType_t xTaskGetCoreID(TaskHandle_t xTask)
{
    (void)xTask;
    return 0;  /* Single-core: always core 0 */
}

TaskHandle_t xTaskGetIdleTaskHandleForCore(BaseType_t xCoreID)
{
    (void)xCoreID;
    return NULL;  /* No idle task in ThreadX compat */
}

TaskHandle_t xTaskGetCurrentTaskHandleForCore(BaseType_t xCoreID)
{
    (void)xCoreID;
    return xTaskGetCurrentTaskHandle();
}

void *pvTaskGetCurrentTCBForCore(BaseType_t xCoreID)
{
    (void)xCoreID;
    return (void *)tx_thread_identify();
}

uint8_t *pxTaskGetStackStart(TaskHandle_t xTask)
{
    if (xTask == NULL) return NULL;
    return (uint8_t *)xTask->thread.tx_thread_stack_start;
}

/* ── Heap management ─────────────────────────────────────────────── */

size_t xPortGetFreeHeapSize(void)
{
    /* Return available bytes from the system heap (approximate) */
    extern TX_BYTE_POOL *_tx_esp32c6_system_byte_pool;
    if (_tx_esp32c6_system_byte_pool == NULL) return 0;
    return (size_t)_tx_esp32c6_system_byte_pool->tx_byte_pool_available;
}

size_t xPortGetMinimumEverFreeHeapSize(void)
{
    /* ThreadX doesn't track minimum — return current */
    return xPortGetFreeHeapSize();
}

bool xPortCheckValidListMem(const void *ptr)  { (void)ptr; return true; }
bool xPortCheckValidTCBMem(const void *ptr)   { (void)ptr; return true; }
bool xPortcheckValidStackMem(const void *ptr) { (void)ptr; return true; }

/* ── Task debug stubs (esp_gdbstub, panic handler) ──────────────\n *
 * These APIs are used by esp_gdbstub to iterate tasks during panic.
 * ThreadX doesn't expose task lists in FreeRTOS format, so we return
 * empty results. Panic handler will still work — just won't show
 * per-task backtraces.
 */

#include "esp_private/freertos_debug.h"

int xTaskGetNext(TaskIterator_t *xIterator)
{
    (void)xIterator;
    return -1;  /* No tasks to iterate */
}

BaseType_t vTaskGetSnapshot(TaskHandle_t pxTask, TaskSnapshot_t *pxTaskSnapshot)
{
    (void)pxTask;
    (void)pxTaskSnapshot;
    return pdFALSE;
}

UBaseType_t uxTaskGetSnapshotAll(TaskSnapshot_t * const pxTaskSnapshotArray,
                                  const UBaseType_t uxArrayLength,
                                  UBaseType_t * const pxTCBSize)
{
    (void)pxTaskSnapshotArray;
    (void)uxArrayLength;
    if (pxTCBSize != NULL) {
        *pxTCBSize = 0;
    }
    return 0;  /* No snapshots */
}

/* ── Stack high water mark stub ─────────────────────────────────── */

UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t xTask)
{
    (void)xTask;
    return 0;  /* ThreadX doesn't track this in FreeRTOS format */
}

/* ── Timeout functions ─────────────────────────────────────────────
 * Used by drivers (usb_serial_jtag, esp_netif) for non-blocking waits.
 * vTaskSetTimeOutState captures current tick count.
 * xTaskCheckForTimeOut checks if the timeout has elapsed.
 */

void vTaskSetTimeOutState(TimeOut_t * const pxTimeOut)
{
    if (pxTimeOut != NULL) {
        pxTimeOut->xOverflowCount = 0;
        pxTimeOut->xTimeOnEntering = xTaskGetTickCount();
    }
}

/* Internal variant — same behavior, no critical section (caller already protected) */
void vTaskInternalSetTimeOutState(TimeOut_t * const pxTimeOut)
{
    vTaskSetTimeOutState(pxTimeOut);
}

BaseType_t xTaskCheckForTimeOut(TimeOut_t * const pxTimeOut,
                                 TickType_t * const pxTicksToWait)
{
    if (pxTimeOut == NULL || pxTicksToWait == NULL) return pdTRUE;

    TickType_t now = xTaskGetTickCount();
    TickType_t elapsed = now - pxTimeOut->xTimeOnEntering;

    if (*pxTicksToWait == portMAX_DELAY) {
        return pdFALSE;  /* Infinite wait never times out */
    }

    if (elapsed >= *pxTicksToWait) {
        *pxTicksToWait = 0;
        return pdTRUE;   /* Timed out */
    }

    *pxTicksToWait -= elapsed;
    pxTimeOut->xTimeOnEntering = now;
    return pdFALSE;  /* Not yet timed out */
}

/* ── Newlib retarget lock overrides ──────────────────────────────
 *
 * Newlib/locks.c provides _lock_init, _lock_acquire, etc. using FreeRTOS
 * semaphore APIs (xSemaphoreTake, xQueueCreateMutex, etc.). Our compat
 * layer translates these to ThreadX underneath. We no longer need our own
 * direct TX_MUTEX-based _lock_* implementations — they would cause
 * duplicate symbol errors with newlib/locks.c.
 */

/* ── FreeRTOS List operations ────────────────────────────────────
 *
 * Used by esp_ringbuf (task blocking on send/receive) and pthread_cond_var.
 * These are pure data structure operations — doubly-linked sorted list.
 */

void vListInitialise(List_t * const pxList)
{
    /* End marker points to itself (empty circular list) */
    pxList->pxIndex = (ListItem_t *)&(pxList->xListEnd);
    pxList->xListEnd.xItemValue = portMAX_DELAY;
    pxList->xListEnd.pxNext = (ListItem_t *)&(pxList->xListEnd);
    pxList->xListEnd.pxPrevious = (ListItem_t *)&(pxList->xListEnd);
    pxList->uxNumberOfItems = 0;
}

void vListInitialiseItem(ListItem_t * const pxItem)
{
    pxItem->pxContainer = NULL;
}

void vListInsert(List_t * const pxList, ListItem_t * const pxNewListItem)
{
    const TickType_t xValueOfInsertion = pxNewListItem->xItemValue;
    ListItem_t *pxIterator;

    /* Find insertion point (sorted ascending by xItemValue) */
    if (xValueOfInsertion == portMAX_DELAY) {
        pxIterator = pxList->xListEnd.pxPrevious;
    } else {
        for (pxIterator = (ListItem_t *)&(pxList->xListEnd);
             pxIterator->pxNext->xItemValue <= xValueOfInsertion;
             pxIterator = pxIterator->pxNext) {
            /* Empty loop body — just walk the list */
        }
    }

    pxNewListItem->pxNext = pxIterator->pxNext;
    pxNewListItem->pxPrevious = pxIterator;
    pxIterator->pxNext->pxPrevious = pxNewListItem;
    pxIterator->pxNext = pxNewListItem;

    pxNewListItem->pxContainer = pxList;
    (pxList->uxNumberOfItems)++;
}

void vListInsertEnd(List_t * const pxList, ListItem_t * const pxNewListItem)
{
    ListItem_t * const pxIndex = pxList->pxIndex;

    pxNewListItem->pxNext = pxIndex;
    pxNewListItem->pxPrevious = pxIndex->pxPrevious;
    pxIndex->pxPrevious->pxNext = pxNewListItem;
    pxIndex->pxPrevious = pxNewListItem;

    pxNewListItem->pxContainer = pxList;
    (pxList->uxNumberOfItems)++;
}

UBaseType_t uxListRemove(ListItem_t * const pxItemToRemove)
{
    List_t * const pxList = pxItemToRemove->pxContainer;

    pxItemToRemove->pxNext->pxPrevious = pxItemToRemove->pxPrevious;
    pxItemToRemove->pxPrevious->pxNext = pxItemToRemove->pxNext;

    if (pxList->pxIndex == pxItemToRemove) {
        pxList->pxIndex = pxItemToRemove->pxPrevious;
    }

    pxItemToRemove->pxContainer = NULL;
    (pxList->uxNumberOfItems)--;
    return pxList->uxNumberOfItems;
}

/* ── Event list stubs ────────────────────────────────────────────
 *
 * In real FreeRTOS, these integrate with the scheduler to block/unblock
 * tasks on event lists. For the ThreadX compat layer, esp_ringbuf calls
 * these to manage task blocking. We provide minimal stubs that compile
 * and won't crash but don't implement real event-list blocking.
 *
 * For WiFi, the critical path is through the compat layer's
 * xSemaphoreTake/xQueueReceive (which use ThreadX primitives). Ring
 * buffers are used by some peripheral drivers but not in the WiFi
 * data path.
 */

void vTaskPlaceOnEventList(List_t * const pxEventList,
                            const TickType_t xTicksToWait)
{
    (void)pxEventList;
    /* Instead of blocking on the event list (which requires deep scheduler
     * integration), just delay. This is approximate but prevents hangs. */
    if (xTicksToWait > 0 && xTicksToWait != portMAX_DELAY) {
        vTaskDelay(xTicksToWait);
    } else if (xTicksToWait == portMAX_DELAY) {
        vTaskDelay(1);  /* Avoid infinite block — caller will retry */
    }
}

BaseType_t xTaskRemoveFromEventList(const List_t * const pxEventList)
{
    (void)pxEventList;
    /* No task was actually placed on the event list, so nothing to remove.
     * Return pdFALSE = no context switch needed. */
    return pdFALSE;
}
