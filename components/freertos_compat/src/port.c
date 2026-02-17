/*
 * port.c — ESP-IDF specific supplements for the ThreadX FreeRTOS compat layer
 *
 * Provides:
 *   - vPortYield (used by portYIELD macro)
 *   - vPortEnterCritical / vPortExitCritical (used by the compat layer)
 *   - xTaskCreatePinnedToCore (ESP-IDF specific, not in upstream compat)
 *   - Newlib retarget lock overrides (ThreadX mutexes instead of FreeRTOS)
 */

#include <tx_api.h>
#include <FreeRTOS.h>
#include <sys/lock.h>
#include <string.h>

void vPortYield(void)
{
    tx_thread_relinquish();
}

/*
 * Critical section functions — called via portENTER_CRITICAL / portEXIT_CRITICAL
 * which are defined in the upstream FreeRTOS.h compat header.
 */
static volatile uint32_t critical_nesting = 0;
static ULONG64 saved_posture;

void vPortEnterCritical(void)
{
    TX_INTERRUPT_SAVE_AREA

    TX_DISABLE

    if (critical_nesting == 0) {
        saved_posture = interrupt_save;
    }
    critical_nesting++;
}

void vPortExitCritical(void)
{
    if (critical_nesting > 0) {
        critical_nesting--;
        if (critical_nesting == 0) {
            ULONG64 interrupt_save = saved_posture;
            TX_RESTORE
        }
    }
}

/*
 * xTaskCreatePinnedToCore — ESP-IDF specific API not in upstream compat.
 * ESP32-C6 is single-core, so we just forward to xTaskCreate.
 */
BaseType_t xTaskCreatePinnedToCore(
    TaskFunction_t pvTaskCode,
    const char * const pcName,
    const configSTACK_DEPTH_TYPE usStackDepth,
    void * const pvParameters,
    UBaseType_t uxPriority,
    TaskHandle_t * const pxCreatedTask,
    const BaseType_t xCoreID)
{
    (void)xCoreID;
    return xTaskCreate(pvTaskCode, pcName, usStackDepth, pvParameters,
                       uxPriority, pxCreatedTask);
}

/*
 * Newlib retarget lock overrides — ThreadX mutexes.
 *
 * ESP-IDF's newlib defines:
 *   typedef struct __lock * _lock_t;
 *   struct __lock { int reserved[21]; };  // sized for a FreeRTOS mutex
 *
 * We store a TX_MUTEX inside struct __lock's reserved space.
 * TX_MUTEX must fit within 21 ints = 84 bytes (TX_MUTEX is ~76 bytes on RV32).
 */

static struct __lock _global_lock_instance;
static int _global_lock_initialized = 0;

static TX_MUTEX *lock_to_mutex(struct __lock *lock)
{
    return (TX_MUTEX *)lock->reserved;
}

void _lock_init(_lock_t *plock)
{
    if (!_global_lock_initialized) {
        memset(&_global_lock_instance, 0, sizeof(_global_lock_instance));
        tx_mutex_create(lock_to_mutex(&_global_lock_instance),
                        "newlib_global", TX_INHERIT);
        _global_lock_initialized = 1;
    }
    *plock = &_global_lock_instance;
}

void _lock_init_recursive(_lock_t *plock)
{
    _lock_init(plock);
}

void _lock_close(_lock_t *plock)
{
    (void)plock;
}

void _lock_close_recursive(_lock_t *plock)
{
    (void)plock;
}

void _lock_acquire(_lock_t *plock)
{
    if (plock && *plock) {
        TX_MUTEX *mtx = lock_to_mutex(*plock);
        tx_mutex_get(mtx, TX_WAIT_FOREVER);
    }
}

void _lock_acquire_recursive(_lock_t *plock)
{
    _lock_acquire(plock);
}

int _lock_try_acquire(_lock_t *plock)
{
    if (plock && *plock) {
        TX_MUTEX *mtx = lock_to_mutex(*plock);
        return (tx_mutex_get(mtx, TX_NO_WAIT) == TX_SUCCESS) ? 0 : -1;
    }
    return -1;
}

int _lock_try_acquire_recursive(_lock_t *plock)
{
    return _lock_try_acquire(plock);
}

void _lock_release(_lock_t *plock)
{
    if (plock && *plock) {
        TX_MUTEX *mtx = lock_to_mutex(*plock);
        tx_mutex_put(mtx);
    }
}

void _lock_release_recursive(_lock_t *plock)
{
    _lock_release(plock);
}
