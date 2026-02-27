/*
 * list.h — FreeRTOS list type definitions for ThreadX compat layer
 *
 * Provides List_t, ListItem_t, MiniListItem_t structures and associated
 * macros/functions. Used by esp_ringbuf, pthread_cond_var, and internally
 * by FreeRTOS for task blocking on event lists.
 *
 * The list data structure itself is simple (doubly-linked sorted list).
 * The complex part is xTaskRemoveFromEventList/vTaskPlaceOnEventList which
 * integrate with the scheduler — those are declared here but implemented
 * as stubs in port.c.
 */

#ifndef INC_FREERTOS_LIST_H
#define INC_FREERTOS_LIST_H

#include "FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── configLIST_VOLATILE — normally empty ──────────────────────── */
#ifndef configLIST_VOLATILE
#define configLIST_VOLATILE
#endif

/* ── Integrity check macros — disabled (no-op) ─────────────────── */
#define listFIRST_LIST_ITEM_INTEGRITY_CHECK_VALUE
#define listSECOND_LIST_ITEM_INTEGRITY_CHECK_VALUE
#define listFIRST_LIST_INTEGRITY_CHECK_VALUE
#define listSECOND_LIST_INTEGRITY_CHECK_VALUE
#define listSET_FIRST_LIST_ITEM_INTEGRITY_CHECK_VALUE(pxItem)
#define listSET_SECOND_LIST_ITEM_INTEGRITY_CHECK_VALUE(pxItem)
#define listSET_LIST_INTEGRITY_CHECK_1_VALUE(pxList)
#define listSET_LIST_INTEGRITY_CHECK_2_VALUE(pxList)
#define listTEST_LIST_ITEM_INTEGRITY(pxItem)
#define listTEST_LIST_INTEGRITY(pxList)

/* ── ListItem_t ────────────────────────────────────────────────── */
struct xLIST_ITEM
{
    configLIST_VOLATILE TickType_t xItemValue;
    struct xLIST_ITEM * configLIST_VOLATILE pxNext;
    struct xLIST_ITEM * configLIST_VOLATILE pxPrevious;
    void * pvOwner;
    struct xLIST * configLIST_VOLATILE pxContainer;
};
typedef struct xLIST_ITEM ListItem_t;

/* ── MiniListItem_t — same as ListItem_t when mini list items disabled */
typedef struct xLIST_ITEM MiniListItem_t;

/* ── List_t ────────────────────────────────────────────────────── */
typedef struct xLIST
{
    volatile UBaseType_t uxNumberOfItems;
    ListItem_t * configLIST_VOLATILE pxIndex;
    MiniListItem_t xListEnd;
} List_t;

/* ── Access macros ─────────────────────────────────────────────── */
#define listSET_LIST_ITEM_VALUE(pxListItem, xValue) \
    ((pxListItem)->xItemValue = (xValue))

#define listGET_LIST_ITEM_VALUE(pxListItem) \
    ((pxListItem)->xItemValue)

#define listSET_LIST_ITEM_OWNER(pxListItem, pxOwner) \
    ((pxListItem)->pvOwner = (void *)(pxOwner))

#define listGET_LIST_ITEM_OWNER(pxListItem) \
    ((pxListItem)->pvOwner)

/* ── List state macros ─────────────────────────────────────────── */
#define listLIST_IS_EMPTY(pxList) \
    (((pxList)->uxNumberOfItems == (UBaseType_t)0) ? pdTRUE : pdFALSE)

#define listCURRENT_LIST_LENGTH(pxList) \
    ((pxList)->uxNumberOfItems)

#define listLIST_IS_INITIALISED(pxList) \
    ((pxList)->xListEnd.xItemValue == portMAX_DELAY)

/* ── Navigation macros ─────────────────────────────────────────── */
#define listGET_HEAD_ENTRY(pxList) \
    (((pxList)->xListEnd).pxNext)

#define listGET_ITEM_VALUE_OF_HEAD_ENTRY(pxList) \
    (((pxList)->xListEnd).pxNext->xItemValue)

#define listGET_NEXT(pxListItem) \
    ((pxListItem)->pxNext)

#define listGET_END_MARKER(pxList) \
    ((ListItem_t const *)(&((pxList)->xListEnd)))

#define listIS_CONTAINED_WITHIN(pxList, pxListItem) \
    (((pxListItem)->pxContainer == (pxList)) ? (pdTRUE) : (pdFALSE))

#define listLIST_ITEM_CONTAINER(pxListItem) \
    ((pxListItem)->pxContainer)

/* ── Iterator macro ────────────────────────────────────────────── */
#define listGET_OWNER_OF_NEXT_ENTRY(pxTCB, pxList)                          \
    do {                                                                     \
        List_t * const pxConstList = (pxList);                               \
        pxConstList->pxIndex = pxConstList->pxIndex->pxNext;                 \
        if ((void *)pxConstList->pxIndex == (void *)&(pxConstList->xListEnd))\
        {                                                                    \
            pxConstList->pxIndex = pxConstList->pxIndex->pxNext;             \
        }                                                                    \
        (pxTCB) = pxConstList->pxIndex->pvOwner;                             \
    } while (0)

/* ── Inline removal macro ─────────────────────────────────────── */
#define listREMOVE_ITEM(pxItemToRemove)                                      \
    do {                                                                      \
        List_t * const pxList = (pxItemToRemove)->pxContainer;                \
        (pxItemToRemove)->pxNext->pxPrevious = (pxItemToRemove)->pxPrevious;  \
        (pxItemToRemove)->pxPrevious->pxNext = (pxItemToRemove)->pxNext;      \
        if (pxList->pxIndex == (pxItemToRemove)) {                            \
            pxList->pxIndex = (pxItemToRemove)->pxPrevious;                   \
        }                                                                     \
        (pxItemToRemove)->pxContainer = NULL;                                 \
        (pxList->uxNumberOfItems)--;                                          \
    } while (0)

/* ── Inline end-insertion macro ────────────────────────────────── */
#define listINSERT_END(pxList, pxNewListItem)                                \
    do {                                                                      \
        ListItem_t * const pxIndex = (pxList)->pxIndex;                       \
        (pxNewListItem)->pxNext = pxIndex;                                    \
        (pxNewListItem)->pxPrevious = pxIndex->pxPrevious;                    \
        pxIndex->pxPrevious->pxNext = (pxNewListItem);                        \
        pxIndex->pxPrevious = (pxNewListItem);                                \
        (pxNewListItem)->pxContainer = (pxList);                              \
        ((pxList)->uxNumberOfItems)++;                                        \
    } while (0)

/* ── Function declarations ─────────────────────────────────────── */
void vListInitialise(List_t * const pxList);
void vListInitialiseItem(ListItem_t * const pxItem);
void vListInsert(List_t * const pxList, ListItem_t * const pxNewListItem);
void vListInsertEnd(List_t * const pxList, ListItem_t * const pxNewListItem);
UBaseType_t uxListRemove(ListItem_t * const pxItemToRemove);

/* ── Event list functions (scheduler integration) ──────────────── */
void vTaskPlaceOnEventList(List_t * const pxEventList,
                            const TickType_t xTicksToWait);
BaseType_t xTaskRemoveFromEventList(const List_t * const pxEventList);

#ifdef __cplusplus
}
#endif

#endif /* INC_FREERTOS_LIST_H */
