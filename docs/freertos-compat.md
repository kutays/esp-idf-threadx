# FreeRTOS Compatibility Layer

## Why It Is Needed

ESP-IDF components (WiFi, BT, logging, networking) use FreeRTOS APIs internally.
They call `xTaskCreate`, `xSemaphoreCreateMutex`, `xQueueCreate`, etc.

Rather than modifying every ESP-IDF component, we provide a compatibility shim
that maps FreeRTOS calls to ThreadX calls. The shim lives in
`components/freertos_compat/`.

## Strategy: Use the Official Upstream Compat Layer

ThreadX ships with an official FreeRTOS compatibility layer at:
```
threadx/utility/rtos_compatibility_layers/FreeRTOS/
├── tx_freertos.c      # 2846-line implementation
├── FreeRTOS.h         # Main FreeRTOS header
├── task.h
├── semphr.h
├── queue.h
├── timers.h
└── event_groups.h
```

We use `tx_freertos.c` directly. Our `freertos_compat/` component wraps it with:
1. `FreeRTOSConfig.h` — configuration file the compat layer reads
2. `portmacro.h` — basic port type definitions
3. `src/port.c` — ESP-IDF-specific additions

## Include Path Strategy

The critical challenge: ESP-IDF's own FreeRTOS component ships its own `FreeRTOS.h`,
`task.h`, etc. When other components include `<freertos/FreeRTOS.h>`, they must
find **our** shim headers, not the real FreeRTOS headers.

CMakeLists.txt for `freertos_compat`:
```cmake
target_include_directories(${COMPONENT_LIB} BEFORE PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${TXFR_DIR}"
)
```

`BEFORE` prepends our include directories to the front of the include search
path. Our `include/` contains `FreeRTOSConfig.h` and `include/freertos/portmacro.h`.
The upstream compat headers (`FreeRTOS.h`, `task.h`, etc.) come from `${TXFR_DIR}`.

When a component does `#include <freertos/FreeRTOS.h>`, it finds the upstream
compat `FreeRTOS.h` before ESP-IDF's real one.

## FreeRTOSConfig.h

Our configuration for the compat layer. Key decisions:

```c
#define TX_FREERTOS_AUTO_INIT   1
```
Tells `tx_freertos.c` to automatically create its internal byte pool during
`tx_application_define`. Without this, the compat layer must be initialized
manually before any FreeRTOS API is called.

```c
// Port macros defined directly — NO #include of portmacro.h
extern void vPortYield(void);
#define portYIELD()             vPortYield()
#define portDISABLE_INTERRUPTS() do { __asm__ volatile("csrc mstatus, 8"); } while(0)
#define portENABLE_INTERRUPTS()  do { __asm__ volatile("csrs mstatus, 8"); } while(0)
```

We define these inline rather than including `portmacro.h` because `portmacro.h`
in ESP-IDF's FreeRTOS defines `BaseType_t`, `UBaseType_t`, `StackType_t`, etc.
Those types conflict with the ones in the upstream compat layer's `FreeRTOS.h`.

The inline CSR instructions:
- `csrc mstatus, 8` — clears bit 3 (MIE) in mstatus, disabling all machine interrupts
- `csrs mstatus, 8` — sets bit 3 (MIE) in mstatus, enabling interrupts

The immediate value `8 = 0b1000 = bit 3` = MIE (Machine Interrupt Enable) in mstatus.

## port.c — ESP-IDF Specific Additions

### vPortYield

```c
void vPortYield(void)
{
    tx_thread_relinquish();
}
```

`tx_thread_relinquish()` yields the current time slice to another thread of
equal priority. If there is no equal-priority ready thread, it returns immediately.

### Critical Sections

```c
static volatile uint32_t critical_nesting = 0;
static ULONG64 saved_posture;

void vPortEnterCritical(void)
{
    TX_INTERRUPT_SAVE_AREA     // declares: ULONG64 interrupt_save;
    TX_DISABLE                 // disables interrupts, saves state in interrupt_save
    if (critical_nesting == 0) {
        saved_posture = interrupt_save;
    }
    critical_nesting++;
}
```

`TX_INTERRUPT_SAVE_AREA` is a macro that declares `ULONG64 interrupt_save`.
`TX_DISABLE` disables interrupts and stores the previous mstatus value in
`interrupt_save` (so we know whether interrupts were enabled before we disabled them).

We only save the first nesting level's state in `saved_posture`. Nested calls
to `vPortEnterCritical` simply increment the counter.

```c
void vPortExitCritical(void)
{
    if (critical_nesting > 0) {
        critical_nesting--;
        if (critical_nesting == 0) {
            ULONG64 interrupt_save = saved_posture;
            TX_RESTORE   // restores interrupts using interrupt_save
        }
    }
}
```

`TX_RESTORE` re-enables interrupts if they were enabled before the critical
section (uses `interrupt_save` to restore the saved mstatus.MIE state).

### xTaskCreatePinnedToCore

ESP-IDF (and therefore many ESP-IDF components) calls this function. It does
not exist in the standard FreeRTOS API or in the compat layer. We provide it:

```c
BaseType_t xTaskCreatePinnedToCore(... xCoreID)
{
    (void)xCoreID;  // ESP32-C6 is single-core
    return xTaskCreate(...);
}
```

`xTaskCreate` is provided by `tx_freertos.c` and maps to `tx_thread_create`.

### Newlib Lock Overrides

ESP-IDF's newlib C library uses locking for thread safety of functions like
`printf`, `malloc`, `strtok`, etc. It defines an abstraction:

```c
// From ESP-IDF newlib:
typedef struct __lock * _lock_t;
struct __lock {
    int reserved[21];  // 84 bytes — sized to hold a FreeRTOS mutex
};
```

ESP-IDF normally fills these with FreeRTOS mutex data. Since we replaced
FreeRTOS with ThreadX, we must provide the `_lock_*` functions.

Our approach: store a `TX_MUTEX` inside the `reserved[21]` array.
`TX_MUTEX` on RV32 is ~76 bytes, which fits within 84 bytes.

```c
static TX_MUTEX *lock_to_mutex(struct __lock *lock)
{
    return (TX_MUTEX *)lock->reserved;
}
```

We cast the `reserved` array to a `TX_MUTEX *` and use it directly. This avoids
any separate allocation.

**Limitation**: The current implementation uses a single global lock instance
rather than per-lock instances. This is a simplification that works for initial
testing but may cause contention in production if many locks are needed
simultaneously.

## Priority Mapping

FreeRTOS priorities and ThreadX priorities are **inverted**:
- FreeRTOS: 0 = lowest, configMAX_PRIORITIES-1 = highest
- ThreadX: 0 = highest, TX_MAX_PRIORITIES-1 = lowest

The `tx_freertos.c` compat layer handles this conversion internally:
```c
threadx_priority = (TX_MAX_PRIORITIES - 1) - freertos_priority
```

With TX_MAX_PRIORITIES=32 and configMAX_PRIORITIES=32:
- FreeRTOS priority 0 → ThreadX priority 31 (lowest)
- FreeRTOS priority 31 → ThreadX priority 0 (highest)

## What tx_freertos.c Provides

The upstream `tx_freertos.c` implements:

| FreeRTOS API          | ThreadX API Used                    |
|-----------------------|-------------------------------------|
| `xTaskCreate`         | `tx_thread_create` + byte pool alloc|
| `vTaskDelete`         | `tx_thread_terminate` + release     |
| `vTaskDelay`          | `tx_thread_sleep`                   |
| `xSemaphoreCreate`    | `tx_semaphore_create`               |
| `xSemaphoreGive/Take` | `tx_semaphore_put/get`              |
| `xMutexCreate`        | `tx_mutex_create(TX_INHERIT)`       |
| `xQueueCreate`        | `tx_queue_create`                   |
| `xTimerCreate`        | `tx_timer_create`                   |
| `xEventGroupCreate`   | `tx_event_flags_create`             |
| `pvPortMalloc`        | `tx_byte_allocate`                  |
| `vPortFree`           | `tx_byte_release`                   |
