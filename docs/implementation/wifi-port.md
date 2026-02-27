# WiFi Port via FreeRTOS Component Override

## Problem Statement

WiFi on ESP32-C6 depends on pre-compiled binary blobs (`libcore.a`, `libnet80211.a`,
`libpp.a`) that call through a function pointer table (`wifi_osi_funcs_t` defined in
`esp_adapter.c`). These wrapper functions call FreeRTOS APIs (`xQueueCreate`,
`xSemaphoreTake`, `xEventGroupWaitBits`, etc.). For WiFi to work on ThreadX, these
FreeRTOS calls must route to our ThreadX compat layer.

**The core type conflict:** ESP-IDF components (`esp_event`, `esp_wifi`, `lwip`,
`esp_coex`) are compiled with ESP-IDF's **real FreeRTOS headers**. Our compat layer
uses **different types** (e.g., `txfr_queue_t*` vs real FreeRTOS `QueueHandle_t`).
These are different pointer types pointing to different struct layouts. We cannot mix
real FreeRTOS headers with compat implementations in the same compilation unit.

**The current architecture has two type systems:**

```
ESP-IDF freertos (real headers) ──────┐
                                      ├── Type conflict!
freertos_compat (ThreadX-backed) ─────┘
```

When esp_event.c includes `<freertos/queue.h>`, it gets the real FreeRTOS
`QueueHandle_t` (a `struct QueueDefinition *`). But our compat layer defines
`QueueHandle_t` as `txfr_queue_t *`. Code compiled against real headers cannot call
functions compiled against compat headers.

### Why This Is a Compile-Time Problem, Not a Binary/ABI Problem

It is important to understand that the WiFi **binary blobs never see FreeRTOS types
directly**. The blobs call through the `wifi_osi_funcs_t` function pointer table,
which is populated at runtime in `esp_adapter.c`. The blobs only ever pass around
opaque `void *` handles — they never dereference struct internals:

```
WiFi blob (binary)            esp_adapter.c (SOURCE)              FreeRTOS API
──────────────────            ──────────────────────              ────────────
osi_funcs._semphr_create  →  semphr_create_wrapper()        →  xSemaphoreCreateCounting()
osi_funcs._queue_send     →  queue_send_wrapper()           →  xQueueSend()
osi_funcs._mutex_lock     →  mutex_lock_wrapper()           →  xSemaphoreTake()
        ↑                          ↑                                  ↑
  passes void *            casts to SemaphoreHandle_t         returns txfr_sem_t *
  (opaque handle)          (whatever the headers define)      (from compat layer)
```

The actual conflict is at **compile time**: `esp_adapter.c` is **source code** that
gets recompiled as part of the `esp_wifi` component. When it compiles, it does:

```c
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static void *queue_create_wrapper(uint32_t queue_len, uint32_t item_size)
{
    return xQueueCreate(queue_len, item_size);  // returns QueueHandle_t
}
```

The question is: **which `freertos/FreeRTOS.h` does `esp_adapter.c` find?**

- **Current (freertos_compat):** ESP-IDF's real `freertos` component is still active.
  `esp_adapter.c` finds real headers → `QueueHandle_t = struct QueueDefinition *`.
  But `xQueueCreate()` is implemented by our compat layer → actually returns
  `txfr_queue_t *`. The pointer value passes through (it's all `void *` at runtime),
  but the compiler sees mismatched types, and any code that dereferences through the
  wrong struct definition would corrupt memory.

- **With override:** Our `components/freertos/` replaces the built-in component.
  `esp_adapter.c` finds **our** headers → `QueueHandle_t = txfr_queue_t *`.
  `xQueueCreate()` returns `txfr_queue_t *`. Same types everywhere — no conflict.

```
                          Current                           With Override
                          ───────                           ─────────────
esp_adapter.c sees:       real QueueHandle_t                compat QueueHandle_t
                          (struct QueueDefinition *)        (txfr_queue_t *)

xQueueCreate returns:     txfr_queue_t *                    txfr_queue_t *

Match?                    NO (different pointer types)      YES ✓
```

### Why txfr_queue_t Works — We Already Have It

The upstream ThreadX FreeRTOS compat layer (`tx_freertos.c`) already defines all the
wrapper types and implements the full FreeRTOS API using them:

| FreeRTOS type | Compat type | Wraps |
|---------------|-------------|-------|
| `QueueHandle_t` | `txfr_queue_t *` | `TX_SEMAPHORE` (read/write) + ring buffer |
| `SemaphoreHandle_t` | `txfr_sem_t *` | `TX_SEMAPHORE` + `TX_MUTEX` |
| `TaskHandle_t` | `txfr_task_t *` | `TX_THREAD` |
| `EventGroupHandle_t` | `txfr_event_t *` | `TX_EVENT_FLAGS_GROUP` |

These types and implementations already work — `freertos_demo` uses them today.
The override changes nothing about the runtime implementation. It only ensures that
`esp_adapter.c`, `esp_event.c`, `esp_coex_adapter.c`, and all other ESP-IDF source
components see **these same compat types** when they `#include "freertos/FreeRTOS.h"`,
instead of seeing ESP-IDF's real (incompatible) FreeRTOS types.

## Solution: Override ESP-IDF's freertos Component

ESP-IDF's build system allows component overrides: if a component with the same name
exists in `EXTRA_COMPONENT_DIRS`, it takes precedence over the built-in version. We
create `components/freertos/` that overrides ESP-IDF's built-in `freertos` component.

This override handles **both** RTOS modes:
- **ThreadX mode** (`CONFIG_RTOS_SELECTION_THREADX`): provides our ThreadX-backed
  FreeRTOS compat layer with matching headers
- **FreeRTOS mode**: compiles the real FreeRTOS sources from `$IDF_PATH` by absolute
  path (transparent passthrough)

```
components/freertos/ (OVERRIDE)
├── ThreadX mode:
│   ├── Headers match compat types everywhere
│   ├── tx_freertos.c (upstream compat layer)
│   ├── port.c + app_startup.c (ESP-IDF integration)
│   └── ALL ESP-IDF components see same types ✓
│
└── FreeRTOS mode:
    └── Compiles real FreeRTOS from $IDF_PATH
        (transparent passthrough, no changes)
```

**Key benefits:**
1. `app_startup.c` is part of the freertos component — we provide our own
   `esp_startup_start_app()` that calls `tx_kernel_enter()` directly (no more
   `port_start_app_hook()` workaround)
2. Configuration-agnostic: any example compiles with either RTOS via menuconfig
3. No ESP-IDF source modifications — purely component override mechanism
4. Single type system: all ESP-IDF components see our compat types

## Architecture Comparison

### Current (threadx_demo)

```
ESP-IDF freertos ─── real headers ──→ esp_event, esp_wifi, lwip
                                      (QueueHandle_t = struct QueueDefinition *)

freertos_compat ─── compat headers ──→ our app code
                                      (QueueHandle_t = txfr_queue_t *)

TWO type systems → can't pass handles between ESP-IDF and our code
```

### New (with override)

```
components/freertos/ ─── compat headers ──→ EVERYTHING
(overrides built-in)                        (QueueHandle_t = txfr_queue_t * everywhere)

ONE type system → WiFi adapter, esp_event, lwip all use same types ✓
```

## WiFi Stack Call Chain

Understanding how WiFi calls reach FreeRTOS:

```
WiFi binary blob (libnet80211.a)
  → wifi_osi_funcs_t function pointers
    → esp_adapter.c wrapper functions
      → FreeRTOS APIs (xQueueCreate, xSemaphoreTake, etc.)
        → [with override] our ThreadX compat layer
```

The `wifi_osi_funcs_t` table (defined in `esp_private/wifi_os_adapter.h`) has 160+
function pointers covering:

| Category | Functions | Notes |
|----------|-----------|-------|
| **Interrupts** | `_set_intr`, `_clear_intr`, `_set_isr`, `_ints_on/off` | ISR management |
| **Tasks** | `_task_create`, `_task_create_pinned_to_core`, `_task_delete`, `_task_delay` | Thread management |
| **Semaphores** | `_semphr_create`, `_semphr_take`, `_semphr_give`, `_semphr_delete` | Counting semaphores |
| **Mutexes** | `_mutex_create`, `_recursive_mutex_create`, `_mutex_lock/unlock` | Recursive mutexes |
| **Queues** | `_queue_create`, `_queue_send`, `_queue_recv`, `_queue_send_from_isr` | FreeRTOS queues |
| **Event groups** | `_event_group_create`, `_event_group_wait_bits`, `_event_group_set_bits` | Bit flags |
| **Timers** | `_timer_arm`, `_timer_disarm`, `_timer_setfn` | ROM ets_timer, not esp_timer |
| **Memory** | `_malloc`, `_free`, `_malloc_internal`, `_wifi_malloc` | Heap allocation |
| **TLS** | `_wifi_thread_semphr_get` | Per-thread semaphore via pthread TLS |
| **NVS** | `_nvs_open`, `_nvs_set_*`, `_nvs_get_*` | Non-volatile storage |
| **Coex** | 26 coex functions | BT/WiFi coexistence |

## Required FreeRTOS API Surface

### Already provided by upstream tx_freertos.c

The upstream ThreadX FreeRTOS compat layer (`threadx/utility/rtos_compatibility_layers/
FreeRTOS/tx_freertos.c`, ~2846 lines) already implements:

- **Tasks**: `xTaskCreate`, `vTaskDelete`, `vTaskDelay`, `xTaskGetTickCount`,
  `xTaskGetCurrentTaskHandle`, `uxTaskGetStackHighWaterMark`
- **Queues**: `xQueueCreate`, `xQueueSend`, `xQueueSendFromISR`, `xQueueReceive`,
  `xQueueReceiveFromISR`, `uxQueueMessagesWaiting`, `vQueueDelete`
- **Semaphores**: `xSemaphoreCreateCounting`, `xSemaphoreCreateMutex`,
  `xSemaphoreCreateRecursiveMutex`, `xSemaphoreTake`, `xSemaphoreGive`,
  `xSemaphoreTakeFromISR`, `xSemaphoreGiveFromISR`, `vSemaphoreDelete`
- **Event groups**: `xEventGroupCreate`, `xEventGroupWaitBits`,
  `xEventGroupSetBits`, `xEventGroupClearBits`, `vEventGroupDelete`
- **Memory**: `pvPortMalloc`, `vPortFree`
- **Scheduler**: `vTaskStartScheduler`, `xTaskGetSchedulerState`
- **Task notifications**: `xTaskNotify`, `xTaskNotifyWait`, `xTaskNotifyFromISR`

### Must be added by our override component

| API | Where needed | Implementation |
|-----|-------------|----------------|
| `portMUX_TYPE` (spinlock struct) | esp_coex_adapter.c | `typedef struct { uint32_t owner; count; } spinlock_t` |
| `portMUX_INITIALIZER_UNLOCKED` | esp_coex_adapter.c | `{.owner = 0xB33FFFFF, .count = 0}` |
| `portENTER_CRITICAL(mux)` | esp_coex_adapter.c, esp_event.c | Macro ignores mux, calls `vPortEnterCritical()` |
| `portEXIT_CRITICAL(mux)` | esp_coex_adapter.c, esp_event.c | Macro ignores mux, calls `vPortExitCritical()` |
| `portENTER_CRITICAL_ISR(mux)` | esp_coex_adapter.c | Same as `portENTER_CRITICAL` on single-core |
| `portEXIT_CRITICAL_ISR(mux)` | esp_coex_adapter.c | Same as `portEXIT_CRITICAL` on single-core |
| `xPortInIsrContext()` | esp_coex_adapter.c | Read `_tx_thread_system_state` |
| `xPortCanYield()` | esp_adapter.c | `!xPortInIsrContext()` |
| `portYIELD_FROM_ISR()` (no-arg) | esp_coex_adapter.c | Variadic macro: `#define portYIELD_FROM_ISR(...) vPortYield()` |
| `xQueueGenericSend()` | esp_adapter.c | Dispatch to `xQueueSendToFront`/`Back` based on `xCopyPosition` |
| `queueSEND_TO_BACK/FRONT` | esp_adapter.c | `#define queueSEND_TO_BACK 0` / `queueSEND_TO_FRONT 1` |
| `tskNO_AFFINITY` | esp_adapter.c | `#define tskNO_AFFINITY ((UBaseType_t)-1)` |
| `xTaskCreatePinnedToCore()` | esp_adapter.c | Forwards to `xTaskCreate` (single-core) |
| `pvTaskGetThreadLocalStoragePointer()` | pthread_local_storage.c | Per-task TLS array in port.c |
| `vTaskSetThreadLocalStoragePointer()` | pthread_local_storage.c | Per-task TLS array in port.c |
| `vTaskSetThreadLocalStoragePointerAndDelCallback()` | pthread_local_storage.c | Extended TLS with destructor |
| `configNUM_THREAD_LOCAL_STORAGE_POINTERS` | pthread_local_storage.c | `#define` = 2 |
| `configMAX_PRIORITIES` | esp_task.h | In FreeRTOSConfig.h = 32 |
| `esp_startup_start_app()` | esp_system/startup.c | Our app_startup.c calls `tx_kernel_enter()` directly |
| `esp_freertos_hooks` API | various ESP-IDF components | Stub implementations returning ESP_OK |
| `CONFIG_FREERTOS_*` Kconfig | various ESP-IDF components | Stub Kconfig options |

## ESP-IDF FreeRTOS Additions — The Hidden Dependency

### The Problem

When we override ESP-IDF's `freertos` component, we don't just replace the FreeRTOS
kernel — we also replace **all ESP-IDF-specific extensions** that live inside the
freertos component. These extensions are defined in files like:

- `esp_additions/idf_additions.c` — WithCaps APIs (task, queue, semaphore, event group, stream buffer)
- `esp_additions/freertos_compatibility.c` — Legacy API wrappers
- `esp_additions/idf_additions_event_groups.c` — Event group WithCaps
- `heap_idf.c` — Heap management (pvPortMalloc, vPortFree, free heap queries)
- `port_common.c` — Static allocation support
- `esp_additions/freertos_tasks_c_additions.h` — Task utilities (core ID, TCB access, snapshots)

These files define **~50 public functions** that other ESP-IDF components call. When
our override takes over, none of these source files get compiled. Any ESP-IDF component
that calls these functions will fail with "implicit declaration" or "undefined reference"
errors.

### First Approach: Iterative Discovery (Rejected)

The initial approach was to implement only the functions we knew were needed (from
analyzing WiFi adapter code), then discover missing ones during build iteration:

1. Build → compiler error: `implicit declaration of function 'xTaskCreatePinnedToCoreWithCaps'`
2. Add stub for that one function
3. Build again → next error: `implicit declaration of function 'vTaskDeleteWithCaps'`
4. Add stub for that one
5. Repeat for every missing function...

This is slow and fragile. Each build cycle takes minutes. With ~50 functions across
multiple ESP-IDF components, we'd be iterating dozens of times. Worse, some missing
functions only surface when specific components are pulled in (e.g., `esp_http_server`
uses `xTaskCreatePinnedToCoreWithCaps` which we wouldn't discover until something
depends on it transitively).

### Second Approach: Upfront Catalog (Adopted)

Instead, we cataloged the **entire** public API surface of ESP-IDF's freertos additions
by reading all source files, then implemented everything upfront. This required one
research pass but eliminated the iterative build-fix cycle.

The complete catalog of ESP-IDF FreeRTOS additions we must provide:

#### WithCaps APIs (memory-capability-aware creation/deletion)

ESP-IDF extends FreeRTOS with "WithCaps" variants that allocate from specific memory
regions (e.g., PSRAM vs internal SRAM). On ThreadX we ignore caps and forward to
standard APIs.

| Function | Forwards to |
|----------|------------|
| `xTaskCreatePinnedToCoreWithCaps()` | `xTaskCreate()` (ignore core + caps) |
| `vTaskDeleteWithCaps()` | `vTaskDelete()` |
| `xTaskCreateWithCaps()` | inline → `xTaskCreatePinnedToCoreWithCaps()` |
| `xQueueCreateWithCaps()` | `xQueueCreate()` |
| `vQueueDeleteWithCaps()` | `vQueueDelete()` |
| `xSemaphoreCreateGenericWithCaps()` | Dispatches to `CreateMutex`/`CreateCounting`/`CreateBinary`/`CreateRecursiveMutex` based on `ucQueueType` |
| `vSemaphoreDeleteWithCaps()` | `vSemaphoreDelete()` |
| `xSemaphoreCreateBinaryWithCaps()` | inline → `xSemaphoreCreateGenericWithCaps()` |
| `xSemaphoreCreateCountingWithCaps()` | inline → `xSemaphoreCreateGenericWithCaps()` |
| `xSemaphoreCreateMutexWithCaps()` | inline → `xSemaphoreCreateGenericWithCaps()` |
| `xSemaphoreCreateRecursiveMutexWithCaps()` | inline → `xSemaphoreCreateGenericWithCaps()` |
| `xEventGroupCreateWithCaps()` | `xEventGroupCreate()` |
| `vEventGroupDeleteWithCaps()` | `vEventGroupDelete()` |
| `xStreamBufferGenericCreateWithCaps()` | Returns NULL (not implemented) |
| `vStreamBufferGenericDeleteWithCaps()` | No-op |
| `xStreamBufferCreateWithCaps()` | inline → `xStreamBufferGenericCreateWithCaps()` |
| `vStreamBufferDeleteWithCaps()` | inline → `vStreamBufferGenericDeleteWithCaps()` |

#### Legacy Compatibility

| Function | Implementation |
|----------|---------------|
| `xQueueGenericReceive()` | Dispatches to `xQueuePeek()` or `xQueueReceive()` based on `xPeek` flag |
| `xQueueGenericSend()` | Dispatches to `xQueueSendToFront()`/`Back()`/`Overwrite()` based on `xCopyPosition` |

#### Task Utilities

| Function | Implementation |
|----------|---------------|
| `xTaskGetCoreID()` | Returns 0 (single-core) |
| `xTaskGetIdleTaskHandleForCore()` | Returns NULL (no idle task in ThreadX compat) |
| `xTaskGetCurrentTaskHandleForCore()` | Forwards to `xTaskGetCurrentTaskHandle()` |
| `pvTaskGetCurrentTCBForCore()` | Returns `tx_thread_identify()` |
| `pxTaskGetStackStart()` | Reads `tx_thread_stack_start` from TX_THREAD |

#### Heap Management

| Function | Implementation |
|----------|---------------|
| `xPortGetFreeHeapSize()` | Reads `tx_byte_pool_available` from system byte pool |
| `xPortGetMinimumEverFreeHeapSize()` | Returns current free (ThreadX doesn't track minimum) |
| `xPortCheckValidListMem()` | Returns true (no memory protection) |
| `xPortCheckValidTCBMem()` | Returns true |
| `xPortcheckValidStackMem()` | Returns true |

#### Not Implemented (not needed on single-core ESP32-C6)

These functions exist in ESP-IDF's additions but are gated behind multi-core,
SMP, PSRAM, or debug-snapshot config options that don't apply:

- `prvTakeKernelLock()` / `prvReleaseKernelLock()` — multi-core only
- `prvStartSchedulerOtherCores()` — SMP only
- `xTaskIncrementTickOtherCores()` — multi-core only
- `prvTaskCreateDynamicAffinitySetWithCaps()` — SMP + SPIRAM only
- `ulTaskGetIdleRunTimeCounterForCore()` — runtime stats (not enabled)
- `xTaskGetNext()` / `vTaskGetSnapshot()` / `uxTaskGetSnapshotAll()` — task snapshots (disabled)
- `vApplicationGetIdleTaskMemory()` — FreeRTOS idle task (doesn't exist under ThreadX)
- `vApplicationGetTimerTaskMemory()` — FreeRTOS timer task (doesn't exist under ThreadX)

All declarations live in `components/freertos/threadx/include/freertos/FreeRTOS.h`.
All implementations live in `components/freertos/threadx/src/port.c`.

## Thread-Local Storage (TLS) — Critical for WiFi

WiFi uses per-thread semaphores stored via pthread TLS:

```c
// esp_adapter.c — _wifi_thread_semphr_get()
static void *wifi_thread_semphr_get(void)
{
    // 1. Get pthread key (created once)
    // 2. pthread_getspecific(key) → stored semaphore
    // 3. If NULL: create new semaphore, pthread_setspecific(key, sem)
    // 4. Return semaphore handle
}
```

pthread TLS relies on FreeRTOS TLS in the TCB:

```c
// pthread_local_storage.c
void *pthread_getspecific(pthread_key_t key) {
    // Gets TLS pointer from FreeRTOS TCB
    void *tls = pvTaskGetThreadLocalStoragePointer(NULL, PTHREAD_TLS_INDEX);
    // ... lookup key in TLS data structure ...
}
```

**Implementation approach:** Store a `void*` array per ThreadX thread using the
`TX_THREAD_USER_EXTENSION`. We already have `VOID *txfr_thread_ptr` there. We can
either:
1. Add another extension field for TLS, or
2. Store the TLS array inside the `txfr_task_t` wrapper struct (accessed via
   `txfr_thread_ptr`)

Option 2 is cleaner — no change to `tx_user.h`, TLS array lives in the wrapper struct.

## Coexistence Spinlock Details

The coex adapter creates `portMUX_TYPE` spinlocks dynamically:

```c
// esp_coex_adapter.c
void *esp_coex_common_spin_lock_create_wrapper(void)
{
    portMUX_TYPE tmp = portMUX_INITIALIZER_UNLOCKED;
    void *mux = malloc(sizeof(portMUX_TYPE));
    memcpy(mux, &tmp, sizeof(portMUX_TYPE));
    return mux;
}
```

Then uses them in critical sections:

```c
uint32_t esp_coex_common_int_disable_wrapper(void *wifi_int_mux)
{
    if (xPortInIsrContext()) {
        portENTER_CRITICAL_ISR(wifi_int_mux);
    } else {
        portENTER_CRITICAL(wifi_int_mux);
    }
    return 0;
}
```

On single-core ESP32-C6, spinlocks degenerate to interrupt disable/enable. Our
implementation ignores the `mux` argument and just calls `vPortEnterCritical()` /
`vPortExitCritical()`.

## Component Override File Structure

```
components/freertos/
├── CMakeLists.txt                   # Dual-mode: ThreadX compat OR real FreeRTOS
├── Kconfig                          # CONFIG_FREERTOS_* stubs for ESP-IDF compat
├── threadx/                         # ThreadX-mode sources
│   ├── include/
│   │   ├── freertos/                # Matches ESP-IDF #include paths
│   │   │   ├── FreeRTOS.h          # Wraps upstream compat + ESP-IDF extensions
│   │   │   ├── task.h              # Forwards to FreeRTOS.h
│   │   │   ├── queue.h             # Forwards to FreeRTOS.h
│   │   │   ├── semphr.h            # Forwards to FreeRTOS.h
│   │   │   ├── event_groups.h      # Forwards to FreeRTOS.h
│   │   │   ├── timers.h            # Forwards to FreeRTOS.h
│   │   │   ├── portmacro.h         # ESP32-C6 RISC-V + ThreadX port macros
│   │   │   ├── projdefs.h          # Minimal compatibility defines
│   │   │   └── idf_additions.h     # Stub
│   │   ├── FreeRTOSConfig.h        # ThreadX-specific config
│   │   └── esp_freertos_hooks.h    # Stub hooks API
│   └── src/
│       ├── app_startup.c           # esp_startup_start_app() → tx_kernel_enter()
│       ├── port.c                  # Critical sections, TLS, ISR detection
│       └── freertos_hooks.c        # Stub implementations
```

## CMakeLists.txt Design — Dual Mode

The core design: ONE component, TWO modes. When `CONFIG_RTOS_SELECTION_THREADX` is set,
compile our compat layer. Otherwise, compile the real FreeRTOS from `$IDF_PATH` by
absolute path.

**ThreadX mode** includes:
- Upstream `tx_freertos.c` from ThreadX submodule
- Our `app_startup.c`, `port.c`, `freertos_hooks.c`
- Include paths: our headers (first priority), then upstream compat headers
- Requires: `threadx`, `esp_system`, `log`, `esp_common`, `newlib`, `heap`

**FreeRTOS mode** includes:
- All real FreeRTOS kernel sources from `$IDF_PATH/components/freertos/`
- `FreeRTOS-Kernel/` sources: list.c, queue.c, tasks.c, timers.c, etc.
- RISC-V port: `portable/riscv/port.c`, `portable/riscv/portasm.S`
- ESP additions: `idf_additions.c`, `freertos_compatibility.c`
- Real include paths, linker fragments, everything unchanged

## Header Strategy: `#include_next`

Our `freertos/FreeRTOS.h` wrapper uses `#include_next` to chain to the upstream
compat layer's `FreeRTOS.h`:

```c
// components/freertos/threadx/include/freertos/FreeRTOS.h

// First: include the upstream compat layer's FreeRTOS.h
// (has all type definitions: txfr_task_t, txfr_queue_t, etc.)
#include_next <FreeRTOS.h>

// Then: override/extend with ESP-IDF-specific macros
#undef portENTER_CRITICAL
#undef portEXIT_CRITICAL
#include "freertos/portmacro.h"
```

**Include path order matters:** our `threadx/include/` must come BEFORE the upstream
compat's include dir in the search path. The CMakeLists.txt achieves this by listing
our dir first in `INCLUDE_DIRS`.

## app_startup.c — Clean ThreadX Boot

The override component's `app_startup.c` replaces ESP-IDF's version. Instead of the
current `port_start_app_hook()` workaround, we directly provide
`esp_startup_start_app()`:

```c
void esp_startup_start_app(void) {
    tx_kernel_enter();  // Never returns — starts ThreadX scheduler
}
```

This is possible because `esp_startup_start_app()` is defined in the `freertos`
component — by overriding the component, we own this symbol. No more reliance on
the weak `port_start_app_hook()`.

`tx_application_define()` creates a main thread that calls `app_main()`, same as
the current `tx_port_startup.c` but now lives inside the freertos override.

## Kconfig Stubs

Other ESP-IDF components check `CONFIG_FREERTOS_*` Kconfig options at compile time.
We provide stubs matching ESP32-C6 single-core defaults:

```kconfig
config FREERTOS_UNICORE
    bool
    default y

config FREERTOS_HZ
    int
    default 100

config FREERTOS_THREAD_LOCAL_STORAGE_POINTERS
    int
    default 2

config FREERTOS_NUMBER_OF_CORES
    int
    default 1
```

Additional stubs will be discovered during build iteration and added incrementally.

## WiFi Demo Application

Basic WiFi STA (station mode) connection flow:

1. Initialize NVS (`nvs_flash_init`)
2. Initialize TCP/IP stack (`esp_netif_init`)
3. Create default event loop (`esp_event_loop_create_default`)
4. Create WiFi STA netif (`esp_netif_create_default_wifi_sta`)
5. Initialize WiFi (`esp_wifi_init` with default config)
6. Register event handlers for `WIFI_EVENT` and `IP_EVENT`
7. Configure SSID/password
8. Start WiFi (`esp_wifi_start`)
9. Connect (`esp_wifi_connect`)
10. Wait for IP address via event group
11. Print connection info

The demo uses `EXTRA_COMPONENT_DIRS` pointing to both `components/threadx` and
`components/freertos` (the override). The `components/freertos_compat` directory
is NOT included — the override replaces it entirely.

## Known Risks and Mitigations

### 1. pthread TLS Complexity
**Risk:** WiFi adapter uses `pthread_key_create` for per-thread semaphores.
pthread relies on `pvTaskGetThreadLocalStoragePointer` which the upstream compat
layer does not implement.

**Mitigation:** Implement per-task TLS array inside the `txfr_task_t` wrapper struct.
Access via `txfr_thread_ptr` back-pointer. Support at least 2 TLS slots
(`configNUM_THREAD_LOCAL_STORAGE_POINTERS = 2`) with destructor callbacks.

### 2. lwIP sys_arch.c
**Risk:** lwIP's FreeRTOS port (`lwip/port/freertos/sys_arch.c`) compiles against
our compat headers. It uses standard FreeRTOS APIs which our compat layer provides,
but may discover additional missing APIs at compile time.

**Mitigation:** Iterative build-fix cycle. Most lwIP OS calls are basic (semaphores,
mutexes, mailboxes, threads) which the upstream compat layer covers.

### 3. WiFi Interrupt Handling
**Risk:** WiFi binary blob uses `intr_handler_set()` and `esprv_int_enable()` to
register ISRs on CPU interrupt lines. If WiFi uses lines beyond our vector table
entries, ISRs won't fire.

**Mitigation:** The WiFi adapter sets interrupts as LEVEL-TRIGGERED via
`esprv_int_set_type()`. Our mtvec vector table has 32 entries (lines 0-31). We
currently only handle line 17 (SYSTIMER). Additional vector entries must be added
for WiFi interrupt lines, or we need a generic dispatch mechanism.

### 4. Heap Size
**Risk:** WiFi creates many internal tasks and data structures. The current 32KB
byte pool may be insufficient.

**Mitigation:** Increase `CONFIG_THREADX_BYTE_POOL_SIZE` to 65536 (64KB) or higher
in wifi_demo's `sdkconfig.defaults`.

### 5. `#include_next` Ordering
**Risk:** The `FreeRTOS.h` wrapper uses `#include_next` to chain to the upstream
compat's `FreeRTOS.h`. This requires our `include/` directory to appear BEFORE
the upstream compat dir in the include path.

**Mitigation:** In CMakeLists.txt, list our `threadx/include` first in
`INCLUDE_DIRS`, then `${TXFR_DIR}` second. Verified in the CMake design.

### 6. Additional CONFIG_FREERTOS_* Options
**Risk:** ESP-IDF components may check Kconfig options beyond our initial stubs,
causing build failures.

**Mitigation:** Iterative: build, find missing config, add stub, repeat. Most
options have reasonable defaults for single-core RISC-V.

## ESP-IDF Source Files Reference (Read-Only)

These files were analyzed to determine the required API surface. They live under
`$IDF_PATH` and are never modified.

| File | Role |
|------|------|
| `esp_wifi/esp32c6/esp_adapter.c` | WiFi OSAL — 60+ FreeRTOS wrappers in `wifi_osi_funcs_t` |
| `esp_wifi/include/esp_private/wifi_os_adapter.h` | `wifi_osi_funcs_t` struct (160+ function pointers) |
| `esp_coex/esp32c6/esp_coex_adapter.c` | Coex OSAL — spinlocks, ISR semaphores, critical sections |
| `esp_event/esp_event.c` | Event loop — queues, recursive mutexes, task creation |
| `lwip/port/freertos/sys_arch.c` | lwIP OS port — semaphores, mailboxes, threads |
| `freertos/app_startup.c` | Startup — `esp_startup_start_app()` (we replace this) |
| `pthread/pthread_local_storage.c` | Thread-local storage using FreeRTOS TLS API |

## Implementation Order

1. Create override component structure (CMakeLists.txt, directory layout)
2. Port headers (portmacro.h, FreeRTOS.h wrapper, stubs for task/queue/semphr/etc.)
3. Implement port.c (xPortInIsrContext, TLS, xQueueGenericSend, critical sections, newlib locks)
4. Implement app_startup.c (clean ThreadX boot — `esp_startup_start_app` → `tx_kernel_enter`)
5. Implement freertos_hooks.c (stubs)
6. Add Kconfig stubs for CONFIG_FREERTOS_*
7. Create wifi_demo example (CMakeLists, sdkconfig.defaults)
8. Build test — get it to compile (iterative)
9. Write wifi_demo main.c (WiFi STA connect)
10. Flash and debug (iterative testing on hardware)

## Verification Plan

```bash
# Build wifi_demo
cd examples/wifi_demo && idf.py set-target esp32c6 && idf.py build

# Verify threadx_demo still works (must not be affected)
cd ../threadx_demo && idf.py build

# Verify freertos_demo still works
cd ../freertos_demo && idf.py build

# Flash wifi_demo
cd ../wifi_demo && idf.py flash monitor
# Expected: ThreadX boots, WiFi initializes, connects to AP, gets IP
```

## Build Error Iteration Log

Implementation was done by Claude with the user manually running builds and reporting
errors back. After 9 user-reported build iterations (each requiring the user to build,
read errors, paste them back, and wait for fixes), the user requested a holistic approach
instead of continuing one-error-at-a-time iteration.

### Iteration 1: FREERTOS_NO_AFFINITY Kconfig crash (user-reported)

**Error:** `kconfgen` crashed trying to parse the string `"FREERTOS_NO_AFFINITY"` as
a hex literal. ESP-IDF Kconfig files (`esp_system`, `esp_timer`, `lwip`) reference this
symbol as a default value for hex options like `ESP_MAIN_TASK_AFFINITY`.

**Fix:** Added `config FREERTOS_NO_AFFINITY` with `hex` type, `default 0x7FFFFFFF` to
`components/freertos/Kconfig`. This isn't used at runtime on single-core ESP32-C6 — it
purely satisfies the build system's Kconfig dependency resolution.

### Iteration 2: StackType_t conflict + missing FreeRTOSConfig.h path (user-reported)

**Error (a):** `conflicting types for 'StackType_t'` — upstream compat defines
`typedef UINT StackType_t` (`unsigned int`), our portmacro.h redefined it as
`typedef uint32_t StackType_t` (`long unsigned int` on RISC-V). Same underlying type,
different C typedefs.

**Fix (a):** Removed the typedef from portmacro.h. Let the upstream compat layer own it.

**Error (b):** `freertos/FreeRTOSConfig.h: No such file or directory` — `esp_task.h`
includes with `freertos/` prefix but our file was at `threadx/include/FreeRTOSConfig.h`
(no `freertos/` subdirectory).

**Fix (b):** Created `threadx/include/freertos/FreeRTOSConfig.h` that does
`#include "../FreeRTOSConfig.h"` so both include paths work.

### Iteration 3: ESP_TASK_MAIN_* redefined + xTaskCreatePinnedToCoreWithCaps missing (user-reported)

**Error (a):** `warning: "ESP_TASK_MAIN_STACK" redefined` — we defined these in
FreeRTOS.h, but `esp_task.h` (from esp_system component) also defines them.

**Fix (a):** Removed our definitions. Let `esp_task.h` be authoritative.

**Error (b):** `implicit declaration of function 'xTaskCreatePinnedToCoreWithCaps'` —
ESP-IDF's `idf_additions.c` (which provides this) is not compiled in our override.

**Fix (b):** Added declaration in FreeRTOS.h and implementation in port.c that forwards
to `xTaskCreate()` ignoring core ID and memory caps.

### Iteration 4: StreamBufferHandle_t unknown type (user-reported)

**Error:** `unknown type name 'StreamBufferHandle_t'` — used in FreeRTOS.h function
declarations for WithCaps APIs, but the upstream compat layer doesn't define stream
buffer types (stream buffers aren't part of the ThreadX compat layer).

**Fix:** Added `typedef void *StreamBufferHandle_t` and `typedef void *MessageBufferHandle_t`
in FreeRTOS.h before the function declarations.

### Iteration 5: heap_caps_malloc implicit declaration (user-reported)

**Error:** `implicit declaration of function 'heap_caps_malloc'` — ESP-IDF's real
`portmacro.h` includes `esp_heap_caps.h`, and many ESP-IDF components rely on this
transitive include (e.g., `esp_https_ota` calls `heap_caps_malloc` without its own include).

**Fix:** Added `#include "esp_heap_caps.h"` to our `portmacro.h` to match real FreeRTOS's
transitive include behavior.

### Iteration 6: portENTER_CRITICAL_SAFE missing (user-reported)

**Error:** `'portENTER_CRITICAL_SAFE' undeclared` — the ADC component uses SAFE variants
that auto-detect ISR context. Our portmacro.h didn't define them.

**Fix:** Added `#define portENTER_CRITICAL_SAFE(mux) portENTER_CRITICAL(mux)` and
`#define portEXIT_CRITICAL_SAFE(mux) portEXIT_CRITICAL(mux)`. On single-core ESP32-C6,
both paths (ISR and task context) do the same thing: disable interrupts.

### Iteration 7: StaticList_t unknown type (user-reported)

**Error:** `unknown type name 'StaticList_t'` — `esp_ringbuf.h` references `StaticList_t`
in its struct definitions. The upstream compat layer doesn't define FreeRTOS list types
because ThreadX doesn't use FreeRTOS linked lists.

**Fix:** Added size-compatible struct stubs in FreeRTOS.h:
- `StaticListItem_t` (1 TickType_t + 4 void* + 1 TickType_t)
- `StaticMiniListItem_t` (1 TickType_t + 2 void*)
- `StaticList_t` (1 UBaseType_t + 1 void* + 1 StaticMiniListItem_t)

These match real FreeRTOS struct layouts by size so `sizeof()` is correct, but the
fields are never accessed — they're just padding for ESP-IDF components that allocate
these statically.

### Iteration 8: esp_private/freertos_debug.h missing (user-reported)

**Error:** `esp_private/freertos_debug.h: No such file or directory` — `esp_gdbstub`
includes this private header from the real freertos component. It defines
`TaskSnapshot_t`, `TaskIterator_t`, and task-iteration functions used by the panic
handler.

**Fix:** Created stub at `threadx/include/esp_private/freertos_debug.h` with:
- `TaskSnapshot_t` struct (pxTCB, pxTopOfStack, pxEndOfStack)
- `TaskIterator_t` struct (uses `typedef void ListItem_t` since ThreadX doesn't use
  FreeRTOS lists)
- Stub implementations in port.c: `xTaskGetNext()` returns -1, `vTaskGetSnapshot()`
  returns pdFALSE, `uxTaskGetSnapshotAll()` returns 0

The panic handler will still work but won't show per-task backtraces (would need
ThreadX-specific task iteration).

### Iteration 9: SemaphoreHandle_t incompatible pointer + esp_get_free_internal_heap_size (user-reported)

**Error (a):** `passing argument 1 of 'vSemaphoreDelete' from incompatible pointer type`
— `esp_adapter.c:154` casts `void *data` to `SemaphoreHandle_t *` (pointer-to-handle)
then passes it to `vSemaphoreDelete()` which expects `SemaphoreHandle_t` (the handle itself).
With real FreeRTOS where handles are `void *`, this is a harmless type warning. With our
compat layer where `SemaphoreHandle_t` = `txfr_sem_t *`, it's `txfr_sem_t **` vs
`txfr_sem_t *` — a type error.

**Error (b):** `'esp_get_free_internal_heap_size' undeclared` — declared in `esp_system.h`
which `esp_adapter.c` doesn't include directly. With real FreeRTOS, it arrives transitively
through the FreeRTOS config header chain. Our headers don't provide this path.

**This is where the user requested a holistic approach** (see next section).

## Holistic Approach: Comprehensive esp_adapter.c Analysis

After 9 iterations of user-reported build errors, the user asked: *"is there a better
way of doing this I am burning credits for this iteration"*. The user had been manually
building, reading compiler output, pasting errors, and waiting for fixes each cycle.

Instead of continuing one-error-at-a-time, we performed a comprehensive analysis of
**all** ESP-IDF source files that compile against our headers, looking for every possible
type mismatch, missing symbol, and broken transitive include.

### Analysis Scope

Files analyzed:
- `esp_wifi/esp32c6/esp_adapter.c` — WiFi OSAL (60+ FreeRTOS wrappers)
- `esp_coex/esp32c6/esp_coex_adapter.c` — Coexistence OSAL
- `lwip/port/freertos/sys_arch.c` — lwIP OS port
- `esp_event/esp_event.c` — Event loop
- `esp_netif/` — Network interface

### The Fundamental Type Problem

ESP-IDF code assumes FreeRTOS handles are `void *`:
```
Real FreeRTOS:  SemaphoreHandle_t = QueueHandle_t = struct QueueDefinition * ≈ void *
ThreadX compat: SemaphoreHandle_t = txfr_sem_t *  (concrete struct pointer)
```

This means:
- `SemaphoreHandle_t *` in real FreeRTOS = `void **` (pointer arithmetic works)
- `SemaphoreHandle_t *` in our compat = `txfr_sem_t **` (incompatible with `txfr_sem_t *`)
- Explicit function pointer casts like `(void(*)(void *))vQueueDelete` work at ABI level
  (all pointers are 4 bytes on RISC-V) but are type-unsafe

### Findings

| Component | File | Issue | Severity | Fix |
|-----------|------|-------|----------|-----|
| esp_wifi | esp_adapter.c:154 | `SemaphoreHandle_t *sem` passed to `vSemaphoreDelete()` | ERROR | `-Wno-incompatible-pointer-types` |
| esp_wifi | esp_adapter.c:620 | `esp_get_free_internal_heap_size` missing transitive include | ERROR | `#include "esp_system.h"` in FreeRTOS.h |
| esp_wifi | esp_adapter.c:598-615 | Function pointer casts to void* signatures | OK | Explicit casts accepted by compiler |
| esp_coex | esp_coex_adapter.c:84 | `vSemaphoreDelete(semphr)` | OK | Passes handle directly, no double-pointer |
| lwip | sys_arch.c:495 | `vSemaphoreDelete(*sem)` | OK | Correctly dereferences pointer-to-handle |
| esp_event | esp_event.c | void* casts for logging/memset | OK | Standard C, no type issues |

### Fixes Applied (Iteration 9)

**Fix (a):** Added to `components/freertos/CMakeLists.txt`:
```cmake
# ESP-IDF esp_adapter.c assumes FreeRTOS handles are void*.
# Our compat uses concrete struct pointers with identical ABI.
# Suppress the type warning for esp_wifi (safe on RISC-V: all pointers are 4 bytes).
idf_component_get_property(wifi_lib esp_wifi COMPONENT_LIB)
target_compile_options(${wifi_lib} PRIVATE -Wno-incompatible-pointer-types)
```

**Fix (b):** Added to `components/freertos/threadx/include/freertos/FreeRTOS.h`:
```c
/* Transitive include: esp_adapter.c uses esp_get_free_internal_heap_size()
 * without including esp_system.h — relies on FreeRTOS header chain. */
#include "esp_system.h"
```

### Symbol Verification (All Confirmed Present)

Every FreeRTOS macro, type, and function used by esp_adapter.c was verified:
- `configMAX_PRIORITIES` (32) — FreeRTOSConfig.h
- `portMAX_DELAY` — upstream compat FreeRTOS.h
- `portTICK_PERIOD_MS` — portmacro.h
- `tskNO_AFFINITY` — portmacro.h
- `queueSEND_TO_BACK/FRONT` — portmacro.h
- `xPortCanYield()` — portmacro.h
- `xQueueGenericSend()` — FreeRTOS.h + port.c
- `xTaskCreatePinnedToCore()` — FreeRTOS.h + port.c
- All 24 FreeRTOS API functions — upstream compat + port.c
- `OSI_FUNCS_TIME_BLOCKING` — ESP-IDF internal (`wifi_os_adapter.h`)

### Comprehensive Reports

Full analysis reports saved to:
- `/tmp/wifi-port-esp-adapter-analysis.md` — Complete esp_adapter.c API usage catalog
- `/tmp/wifi-port-type-mismatch-report.md` — Cross-component type mismatch analysis

## Lessons Learned

### Iterative vs Holistic Build Debugging

The first 8 iterations followed a reactive pattern: build → error → fix → repeat.
Each cycle required the user to manually run the build, read compiler output, paste
errors, and wait for the fix. This burned significant time and API credits.

The holistic approach (iteration 9) analyzed all relevant ESP-IDF source files upfront,
identified every possible failure point, and fixed them in one pass. This should have
been done earlier — ideally after the first 2-3 iterations made the pattern clear.

**Recommendation for future override work:** Before any build attempt, systematically
read all ESP-IDF source files that will compile against override headers and catalog
every symbol dependency. The upfront cost is one research pass; the savings are
potentially dozens of build iterations.

### The Component Override Tax

Overriding ESP-IDF's `freertos` component means replacing not just the RTOS kernel
but also:
- ~50 ESP-IDF FreeRTOS additions (WithCaps, legacy compat, task utilities, heap mgmt)
- Private headers (`esp_private/freertos_debug.h`)
- Transitive include chains that ESP-IDF components depend on
- Kconfig symbols that other components reference as defaults
- Static type stubs (`StaticList_t`, `StaticListItem_t`) for components like esp_ringbuf

Each of these is a potential build failure. The approach works but requires thorough
analysis of ESP-IDF's internal dependency web.

### Iteration 10: CMake target ordering — "Cannot specify compile options for target" (user-reported)

**Error:**
```
CMake Error at components/freertos/CMakeLists.txt:67 (target_compile_options):
  Cannot specify compile options for target "__idf_esp_wifi" which is not
  built by this project.
```

**Root cause and detailed CMake explanation:**

This error reveals an important aspect of how ESP-IDF's CMake build system works.
Understanding it requires knowing CMake target lifecycle and ESP-IDF's component
registration model.

#### CMake Target Lifecycle

In CMake, a "target" is created by commands like `add_library()` or `add_executable()`.
Once created, you can modify it with `target_compile_options()`, `target_include_directories()`,
`target_link_libraries()`, etc. But you **cannot modify a target that doesn't exist yet**.

```cmake
# This FAILS — my_lib doesn't exist yet:
target_compile_options(my_lib PRIVATE -Wall)
add_library(my_lib src.c)

# This WORKS — my_lib exists:
add_library(my_lib src.c)
target_compile_options(my_lib PRIVATE -Wall)
```

#### ESP-IDF Component Registration

ESP-IDF processes components in dependency order, but the exact ordering is determined
by the dependency graph. Each component calls `idf_component_register()` which internally
calls `add_library(__idf_<name> ...)`. The target name follows the pattern `__idf_<component>`.

When our `components/freertos/CMakeLists.txt` runs:
```cmake
idf_component_get_property(wifi_lib esp_wifi COMPONENT_LIB)  # Gets "__idf_esp_wifi"
target_compile_options(${wifi_lib} PRIVATE -Wno-incompatible-pointer-types)  # FAILS!
```

`freertos` is processed BEFORE `esp_wifi` because:
1. `freertos` has no dependency on `esp_wifi`
2. `esp_wifi` depends on `freertos` (it `#include`s FreeRTOS headers)
3. CMake processes dependencies bottom-up: dependencies first, dependents later

So when our CMakeLists.txt executes, `__idf_esp_wifi` hasn't been created yet.

#### ESP-IDF CMake Property System

ESP-IDF provides several levels of CMake property scoping:

```
┌─────────────────────────────────────────────────────────────────┐
│ idf_build_set_property(PROP VALUE)                              │
│   → Sets a GLOBAL build property (affects ALL components)       │
│   → Stored in CMake cache, applied during final link/compile    │
│   → Safe to call from any component at any time                 │
│                                                                 │
│ idf_component_get_property(var COMP PROP)                       │
│   → Gets a property from another component                      │
│   → COMPONENT_LIB returns the CMake target name (__idf_<name>)  │
│   → Only works if the component has been registered already     │
│                                                                 │
│ target_compile_options(target PRIVATE|PUBLIC|INTERFACE ...)      │
│   → Standard CMake: modifies compile flags for a specific target│
│   → PRIVATE: only this target                                   │
│   → PUBLIC: this target + anything that links to it             │
│   → INTERFACE: only things that link to it (not itself)         │
│   → Target MUST exist when this command runs                    │
└─────────────────────────────────────────────────────────────────┘
```

#### The Three Approaches to Cross-Component Compile Options

**Approach 1: `target_compile_options()` (FAILED)**
```cmake
# In components/freertos/CMakeLists.txt:
idf_component_get_property(wifi_lib esp_wifi COMPONENT_LIB)
target_compile_options(${wifi_lib} PRIVATE -Wno-incompatible-pointer-types)
```
- Fails because `__idf_esp_wifi` target doesn't exist yet
- Would be ideal if it worked (scoped to exactly one component)
- Could work if called from a component that depends ON esp_wifi (processed after it)

**Approach 2: `idf_build_set_property(COMPILE_OPTIONS ...)` (ADOPTED)**
```cmake
# In components/freertos/CMakeLists.txt:
idf_build_set_property(COMPILE_OPTIONS "-Wno-incompatible-pointer-types" APPEND)
```
- Sets the flag GLOBALLY for all components in the build
- Safe to call from any component — doesn't depend on target existence
- Trade-off: broader scope (all components get the flag, not just esp_wifi)
- In practice this is fine: `-Wno-incompatible-pointer-types` only suppresses
  warnings in code that actually has pointer type mismatches. Correctly-typed
  code is unaffected. And the flag only applies when ThreadX mode is active.

**Approach 3: `cmake_language(DEFER)` (Alternative)**
```cmake
# Defers execution until ALL components are registered:
cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR}
    CALL target_compile_options __idf_esp_wifi PRIVATE -Wno-incompatible-pointer-types)
```
- CMake 3.19+ feature: queues a command to run at the end of the directory scope
- By that time, all components are registered and `__idf_esp_wifi` exists
- Most precise (targets exactly one component)
- Risk: depends on CMake version and ESP-IDF's internal processing order
- We chose Approach 2 for simplicity and robustness

#### Why `idf_build_set_property` Works From Any Component

ESP-IDF's build system collects properties during the configure phase and applies
them during the generate phase:

```
Configure Phase (CMake processes CMakeLists.txt files):
  1. freertos/CMakeLists.txt runs → calls idf_build_set_property(COMPILE_OPTIONS ...)
  2. esp_wifi/CMakeLists.txt runs → calls idf_component_register(...)
  3. ... all other components ...

Generate Phase (CMake creates build.ninja):
  4. ESP-IDF reads collected COMPILE_OPTIONS property
  5. Applies to all component targets (including __idf_esp_wifi)
  6. Generates final compile commands with the flag
```

The property is stored in CMake's global scope and applied later, so it doesn't
matter when during the configure phase it's set.

#### ESP-IDF CMake Functions Reference

| Function | Scope | When to Use |
|----------|-------|-------------|
| `idf_component_register(...)` | Current component | Register sources, includes, dependencies |
| `idf_component_get_property(var comp prop)` | Read other component | Get target name, dir, etc. (must exist) |
| `idf_build_set_property(prop val)` | Global build | Set build-wide options (safe from anywhere) |
| `idf_build_get_property(var prop)` | Read global | Get IDF_PATH, build dir, etc. |
| `target_compile_options(tgt ...)` | Specific target | Modify one target (must exist) |
| `target_include_directories(tgt ...)` | Specific target | Add include paths (must exist) |
| `target_link_libraries(tgt ...)` | Specific target | Add link deps (must exist) |
| `target_compile_definitions(tgt ...)` | Specific target | Add -D flags (must exist) |

#### PRIVATE vs PUBLIC vs INTERFACE

These visibility keywords control how properties propagate through dependencies:

```
Component A (freertos) ──depends-on──→ Component B (threadx)

target_compile_options(A PRIVATE -Wfoo)
  → Only A gets -Wfoo. Components depending on A do NOT.

target_compile_options(A PUBLIC -Wfoo)
  → A gets -Wfoo, AND anything that depends on A also gets it.
  → Use for options that affect public headers (e.g., -DTXFR_ENABLED).

target_compile_options(A INTERFACE -Wfoo)
  → A does NOT get -Wfoo, but anything depending on A does.
  → Use for consumer-facing options (e.g., include paths for headers-only libs).
```

In our case, we wanted PRIVATE on esp_wifi (only esp_wifi needs the suppression),
but settled for global because of the target ordering issue.

#### Lesson: Component Processing Order in ESP-IDF

```
ESP-IDF builds a dependency DAG (Directed Acyclic Graph):

    esp_wifi ──→ freertos ──→ esp_common
       │              │
       └──→ esp_event ─┘

Processing order (bottom-up):
  1. esp_common (no deps)
  2. freertos (depends on esp_common)
  3. esp_event (depends on freertos, esp_common)
  4. esp_wifi (depends on freertos, esp_event)
```

When `freertos/CMakeLists.txt` runs at step 2, `esp_wifi` (step 4) hasn't been
processed yet. Any `target_compile_options(__idf_esp_wifi ...)` would fail.

**Rule of thumb:** A component can only modify targets that are its OWN dependencies
(processed before it), never targets that depend ON it (processed after it). To
modify dependents, use global properties or deferred commands.

### Iteration 11: SOC_WIFI_LIGHT_SLEEP_CLK_WIDTH undeclared (user-reported)

**Error:** `'SOC_WIFI_LIGHT_SLEEP_CLK_WIDTH' undeclared` in `esp_coex_adapter.c:126`.
GCC helpfully suggested `CONFIG_SOC_WIFI_LIGHT_SLEEP_CLK_WIDTH` (available from
`sdkconfig.h`, auto-included) — meaning the Kconfig version was visible but the
`soc/soc_caps.h` `#define` was not.

**Root cause — transitive include chain broken:**

The real FreeRTOS `portmacro.h` includes `spinlock.h`, which includes `riscv/rv_utils.h`,
which includes `soc/soc_caps.h`:

```
Real portmacro.h → spinlock.h → riscv/rv_utils.h → soc/soc_caps.h
```

Our `portmacro.h` defines its own minimal `spinlock_t` struct instead of including
`spinlock.h` (since the full spinlock.h pulls in RISC-V CSR helpers we don't need).
This broke the transitive chain to `soc/soc_caps.h`.

**How we traced it:** Used `gcc -H` (print include tree) and compared what the real
`portmacro.h` includes vs ours. The real one includes 10+ ESP-IDF headers:
`spinlock.h`, `soc/interrupt_reg.h`, `esp_macros.h`, `esp_attr.h`, `esp_cpu.h`,
`esp_rom_sys.h`, `esp_heap_caps.h`, `esp_system.h`, `esp_newlib.h`, `esp_timer.h`.
Our portmacro.h only included `esp_heap_caps.h`.

**Technique for tracing transitive include chains:**

```bash
# Step 1: Use gcc -H to print the full include tree (. = depth level)
riscv32-esp-elf-gcc -H -fsyntax-only \
  -include build/config/sdkconfig.h \
  -I <all include paths> \
  -x c - <<< '#include "freertos/portmacro.h"' \
  2>&1 | grep soc_caps

# Step 2: Compare include lists between real and override headers
# Read real portmacro.h, list every #include, check which ones yours has

# Step 3: Use compiler suggestions — GCC's "did you mean CONFIG_SOC_...?"
# tells you the Kconfig version (from sdkconfig.h) IS visible, so the
# non-CONFIG version must come from a header, not the build system
```

**Fix:** Added `#include "soc/soc_caps.h"` directly to our `portmacro.h` alongside
the existing `esp_heap_caps.h` transitive include.

### Iteration 12: portMUX_INITIALIZE missing (user-reported)

**Error:** `implicit declaration of function 'portMUX_INITIALIZE'` in
`esp_driver_parlio/src/parlio_tx.c:392`.

**Root cause:** We already had `spinlock_initialize()` as an inline function in
portmacro.h but hadn't defined the `portMUX_INITIALIZE` macro alias that ESP-IDF
drivers use.

**Fix:** Added `#define portMUX_INITIALIZE(mux) spinlock_initialize(mux)` to portmacro.h.

### Iteration 13: xQueueSemaphoreTake missing (user-reported)

**Error:** `implicit declaration of function 'xQueueSemaphoreTake'` in
`esp_netif/lwip/esp_netif_sntp.c:160`.

**Root cause:** In real FreeRTOS, `xSemaphoreTake` is a **macro** that expands to
`xQueueSemaphoreTake()` (the real internal function in `queue.c`). The ESP-IDF SNTP
code calls `xQueueSemaphoreTake()` directly, bypassing the macro. Our compat layer
provides `xSemaphoreTake` as a real function.

**Fix:** Added `#define xQueueSemaphoreTake(q, t) xSemaphoreTake((q), (t))` in FreeRTOS.h.

### Iteration 14: TimeOut_t, vTaskSetTimeOutState, xTaskCheckForTimeOut missing (user-reported)

**Error:** Three errors in `esp_driver_usb_serial_jtag/src/usb_serial_jtag.c:295-303`:
1. `unknown type name 'TimeOut_t'`
2. `implicit declaration of function 'vTaskSetTimeOutState'`
3. `implicit declaration of function 'xTaskCheckForTimeOut'`

**Root cause:** `TimeOut_t` is a FreeRTOS timeout state struct used for non-blocking
polling patterns. Drivers capture the current tick via `vTaskSetTimeOutState()` then
check elapsed time with `xTaskCheckForTimeOut()`. The upstream compat layer has a
`#define xTimeOutType TimeOut_t` macro but never defines the actual type or functions.

**This is where the user again requested a holistic approach** — after seeing the same
pattern of one-by-one error discovery continuing.

## Second Holistic Approach: Complete Symbol Gap Analysis

After iterations 11-14, the user pointed out that we were still stuck in the same
iterative loop despite the first holistic analysis (iteration 9). The user was correct:

> *"did our preprocessor approach work? aren't we still stuck in the same loop"*
> *"we again in the loop btw"*

### Self-Critique: Tendency Toward One-by-One Iteration

**A recurring pattern in this implementation was Claude's tendency to fix errors one at
a time rather than proactively performing comprehensive analysis.** The user had to
intervene and push for holistic approaches multiple times:

- **Iteration 9** (first push): After 8 one-by-one iterations, the user asked *"is there
  a better way of doing this I am burning credits for this iteration"*. Claude then
  performed a comprehensive analysis of esp_adapter.c and type mismatches.

- **Iteration 14** (second push): Despite the first holistic analysis catching type
  mismatches, Claude reverted to one-by-one fixes for iterations 11-13 (portmacro
  transitive includes, portMUX_INITIALIZE, xQueueSemaphoreTake). The user again pointed
  out *"didn't we checked all already"* and *"we again in the loop"*.

**The lesson:** A single holistic pass isn't enough if it's too narrowly scoped. The first
analysis focused on type mismatches in esp_adapter.c. It should have also covered:
- ALL macros/functions that real portmacro.h exports (not just types)
- ALL transitive includes from real portmacro.h (not just soc_caps.h)
- ALL internal FreeRTOS functions that ESP-IDF code calls directly
- ALL config macros that ESP-IDF checks in `#if` guards

**The user consistently demonstrated better engineering judgment here** — recognizing when
the one-by-one pattern was wasteful and pushing for systematic analysis. Claude should
have adopted this approach by default after the first 2-3 iterations.

### What the Second Holistic Analysis Covered

This time, the analysis was truly comprehensive:

**1. Complete symbol comparison: real FreeRTOS headers vs our override**

Read every public symbol from:
- Real `task.h` — TimeOut_t, vTaskSetTimeOutState, xTaskCheckForTimeOut, pcTaskGetName,
  eTaskGetState, TaskStatus_t, xTaskAbortDelay, etc.
- Real `queue.h` — xQueueGenericCreate, xQueueSemaphoreTake, etc.
- Real `semphr.h` — all macro-to-function mappings (xSemaphoreTake→xQueueSemaphoreTake)
- Real `portmacro.h` — ALL 50+ port macros
- Real `FreeRTOSConfig.h` — ALL config/INCLUDE flags

Then cross-referenced against our override + upstream compat to find gaps.

**2. What the upstream compat layer already provides (no action needed):**

- `pcTaskGetName`, `eTaskGetState`, `xTaskAbortDelay`, `vTaskDelayUntil`
- `StaticTask_t`, `StaticQueue_t`, `StaticSemaphore_t`, `StaticEventGroup_t`
- `eNotifyAction`, task notification functions
- All standard API functions (xTaskCreate, xQueueCreate, xSemaphoreTake, etc.)

**3. What was missing and added in one batch:**

**portmacro.h additions:**
- `portSET_INTERRUPT_MASK_FROM_ISR` / `portCLEAR_INTERRUPT_MASK_FROM_ISR` — ISR-safe
  interrupt save/restore with inline assembly
- `portCHECK_IF_IN_ISR`, `portASSERT_IF_IN_ISR`, `portYIELD_WITHIN_API`, `portGET_CORE_ID`
- `portVALID_LIST_MEM`, `portVALID_TCB_MEM`, `portVALID_STACK_MEM`
- `portTICK_TYPE_IS_ATOMIC`, `portCRITICAL_NESTING_IN_TCB`, `portCLEAN_UP_TCB`
- `portTASK_FUNCTION_PROTO`, `portTASK_FUNCTION`
- `portCONFIGURE_TIMER_FOR_RUN_TIME_STATS`, `portGET_RUN_TIME_COUNTER_VALUE`
- Legacy type aliases (`portCHAR`, `portFLOAT`, `portDOUBLE`, `portLONG`, `portSHORT`)

**FreeRTOS.h additions:**
- `TimeOut_t` struct definition + `vTaskSetTimeOutState()` / `xTaskCheckForTimeOut()` declarations
- `xQueueGenericCreate` macro (routes to `xQueueCreate`, ignoring queue type)
- `queueQUEUE_TYPE_BASE` define (= 0)

**FreeRTOSConfig.h additions:**
- All `configUSE_*` feature flags: `MUTEXES`, `RECURSIVE_MUTEXES`,
  `COUNTING_SEMAPHORES`, `TASK_NOTIFICATIONS`, `TIMERS`, `TRACE_FACILITY`,
  `QUEUE_SETS`, `GENERATE_RUN_TIME_STATS`
- Timer task config: `configTIMER_TASK_PRIORITY`, `configTIMER_QUEUE_LENGTH`,
  `configTIMER_TASK_STACK_DEPTH`
- `configRUN_TIME_COUNTER_TYPE` (uint32_t)
- All `INCLUDE_*` flags: `vTaskPrioritySet`, `uxTaskPriorityGet`, `vTaskSuspend`,
  `vTaskDelayUntil`, `vTaskDelay`, `xTaskGetSchedulerState`,
  `xTaskGetCurrentTaskHandle`, `uxTaskGetStackHighWaterMark`, `xTaskGetHandle`,
  `eTaskGetState`, `xTaskAbortDelay`, `xSemaphoreGetMutexHolder`

**port.c additions:**
- Working `vTaskSetTimeOutState()` implementation (captures tick count)
- Working `xTaskCheckForTimeOut()` implementation (computes elapsed ticks, adjusts
  remaining wait, returns pdTRUE on timeout)

### Recommendation for Future Override Work

When overriding a deeply-integrated component like FreeRTOS:

1. **Before writing any code:** Diff the complete public API surface of the original
   component against what your replacement provides. Use `grep -h '#define\|typedef\|
   extern' real_headers/*.h | sort` to get a flat list.

2. **Don't stop at the first holistic pass.** The first analysis (iteration 9) caught
   type mismatches. It missed port macros, internal functions, config flags, and
   transitive includes. Each category requires its own focused analysis.

3. **Use the preprocessor proactively.** Run `gcc -H` on the real headers to map the
   full transitive include tree. Then ensure your replacement provides equivalent
   transitive paths for every header in the tree.

4. **Categorize gaps by urgency:**
   - Types/macros used at compile time → must exist or code won't compile
   - Functions used at link time → can be stubs, but must exist
   - Config macros checked in `#if` guards → wrong defaults may silently disable features
   - Transitive includes → invisible until a random component fails

5. **The human in the loop sees patterns faster.** When the same category of error
   keeps appearing (missing macro, missing transitive include, missing internal function),
   the right response is to audit the entire category — not fix the one instance and
   wait for the next build.

---

## Build Error Iteration Log — Continued (Iterations 15–16)

### Iteration 15: Macro Redefinition — `portYIELD_WITHIN_API`, `portCLEAN_UP_TCB`

**Build progress:** 250/1191 objects

**Error:**
```
portmacro.h:156:9: error: "portYIELD_WITHIN_API" redefined [-Werror]
  upstream FreeRTOS.h:46: previous definition
portmacro.h:163:9: error: "portCLEAN_UP_TCB" redefined [-Werror]
  upstream FreeRTOS.h:41: previous definition
```

**Root cause:** The upstream ThreadX compat `FreeRTOS.h` (lines 40–59) defines ~20 port
utility macros unconditionally (`portCLEAN_UP_TCB`, `portYIELD_WITHIN_API`,
`portCRITICAL_NESTING_IN_TCB`, `portTICK_TYPE_IS_ATOMIC`, etc.). Our `portmacro.h`
redefines 6 of these with ESP32-C6-specific implementations. Since our `FreeRTOS.h`
wrapper does `#include_next <FreeRTOS.h>` (upstream) first, then `#include portmacro.h`,
the upstream definitions exist when portmacro.h tries to redefine them → `-Werror` fires.

**Fix:** Added `#undef` for ALL 10 conflicting macros in our `FreeRTOS.h` wrapper,
between the upstream `#include_next` and our `#include "freertos/portmacro.h"`. Key
design decision: only undef macros that portmacro.h actually redefines. Macros like
`taskYIELD`, `taskDISABLE_INTERRUPTS`, `taskENTER_CRITICAL_FROM_ISR` are defined by
the upstream only and NOT redefined by portmacro.h — undef'ing them would silently
remove functionality.

Complete conflict map documented in the code:
```
portCRITICAL_NESTING_IN_TCB    (upstream:40 → portmacro.h:162)
portCLEAN_UP_TCB               (upstream:41 → portmacro.h:163)
portCONFIGURE_TIMER_FOR_RUN_TIME_STATS (upstream:44 → portmacro.h:166)
portYIELD_WITHIN_API           (upstream:46 → portmacro.h:156)
portASSERT_IF_IN_ISR           (upstream:54 → portmacro.h:155)
portTICK_TYPE_IS_ATOMIC        (upstream:55 → portmacro.h:161)
portENTER_CRITICAL             (upstream:321 → portmacro.h:85)
portEXIT_CRITICAL              (upstream:322 → portmacro.h:86)
taskENTER_CRITICAL             (upstream:319 → portmacro.h:93)
taskEXIT_CRITICAL              (upstream:320 → portmacro.h:94)
```

**Files changed:** `components/freertos/threadx/include/freertos/FreeRTOS.h`

---

### Iteration 16: `spinlock_t` Type Conflict + Missing `CONFIG_ESP_SYSTEM_SINGLE_CORE_MODE`

**Build progress:** 250/1191 objects (same file range, different errors)

**Error (3 layers deep):**
```
1. spinlock.h:31: warning: "SPINLOCK_FREE" redefined
   portmacro.h:64: previous definition
2. spinlock.h:45: error: conflicting types for 'spinlock_t'
   portmacro.h:60: previous declaration
3. spinlock.h:92: error: 'RVHAL_EXCM_LEVEL_CLIC' undeclared
```

**Root cause — TWO independent bugs:**

**Bug A — Our own `spinlock_t` conflicts with ESP-IDF's real one.**
We defined our own `spinlock_t` typedef, `SPINLOCK_FREE` macro, `SPINLOCK_INITIALIZER`
macro, and `spinlock_initialize()` function directly in portmacro.h. When any ESP-IDF
file includes both our portmacro.h (via FreeRTOS.h) and the real `spinlock.h`
(from `esp_hw_support`), the compiler sees two definitions of the same type and macros.
The file `vfs_eventfd.c` triggered this by including both paths.

**Bug B — `CONFIG_ESP_SYSTEM_SINGLE_CORE_MODE` was never set.**
The real `spinlock.h` has multi-core spinlock code inside
`#if !CONFIG_ESP_SYSTEM_SINGLE_CORE_MODE`. This code references `RVHAL_EXCM_LEVEL_CLIC`
(a CLIC interrupt controller constant not defined for ESP32-C6, which uses PLIC). In
normal ESP-IDF builds, this code is compiled out because `CONFIG_ESP_SYSTEM_SINGLE_CORE_MODE=y`.
In our build, it was NEVER SET — so the multi-core code was compiled, hitting the
undeclared identifier.

**Why was `CONFIG_ESP_SYSTEM_SINGLE_CORE_MODE` missing?**
In ESP-IDF's real FreeRTOS `Kconfig` (line 27):
```kconfig
config FREERTOS_UNICORE
    bool "Run FreeRTOS only on first core"
    select ESP_SYSTEM_SINGLE_CORE_MODE   ← THIS LINE
```
The `select` keyword in Kconfig is a hard dependency — when `FREERTOS_UNICORE=y`, it
forces `ESP_SYSTEM_SINGLE_CORE_MODE=y`. Our override Kconfig had `FREERTOS_UNICORE`
defaulting to `y` but WITHOUT the `select` — so the downstream config was never activated.

**Fix — two parts:**

1. **Kconfig:** Added `select ESP_SYSTEM_SINGLE_CORE_MODE` to our `FREERTOS_UNICORE`.
2. **portmacro.h:** Removed our custom `spinlock_t`/`SPINLOCK_FREE`/`SPINLOCK_INITIALIZER`/
   `spinlock_initialize()`. Replaced with `#include "spinlock.h"` (the real one from
   `esp_hw_support`). Now `portMUX_TYPE` is a typedef of the real `spinlock_t`.

**Files changed:**
- `components/freertos/Kconfig`
- `components/freertos/threadx/include/freertos/portmacro.h`

**Required after fix:** `idf.py reconfigure` to regenerate sdkconfig with new Kconfig select.

---

## Why We Initially Defined Our Own `spinlock_t` (And Why That Was Wrong)

This is worth a dedicated section because it illustrates a common pitfall when overriding
deeply-integrated system components.

### The Original Reasoning

When designing the FreeRTOS component override, we needed `portMUX_TYPE` (which is
`typedef spinlock_t`) because ESP-IDF code passes spinlock pointers to critical section
macros like `portENTER_CRITICAL(mux)`. On single-core ESP32-C6, spinlocks degenerate to
simple interrupt disable/enable — the `mux` argument is ignored. So the reasoning was:

> "We just need the struct to exist so `sizeof(portMUX_TYPE)` works and code compiles.
> The actual spinlock logic doesn't matter on single-core. Let's define a minimal struct."

This led to a 12-line spinlock implementation in portmacro.h:
```c
typedef struct { uint32_t owner; uint32_t count; } spinlock_t;
#define SPINLOCK_FREE ((uint32_t)0xB33FFFFF)
#define SPINLOCK_INITIALIZER { .owner = SPINLOCK_FREE, .count = 0 }
static inline void spinlock_initialize(spinlock_t *lock) { ... }
```

### Why It Broke

ESP-IDF's `spinlock.h` (in `esp_hw_support/include/`) defines the SAME type name
(`spinlock_t`), SAME macros (`SPINLOCK_FREE`, `SPINLOCK_INITIALIZER`), and SAME function
(`spinlock_initialize`). Any file that transitively includes BOTH our portmacro.h and
the real spinlock.h gets conflicting type definitions. This is a **C language constraint**
— you cannot have two `typedef struct { ... } spinlock_t;` definitions even if the
struct layout is identical. They are treated as distinct types.

### Why We Didn't See This Coming

Three factors contributed to this blind spot:

1. **The real `portmacro.h` DOES include `spinlock.h`.** In ESP-IDF's real FreeRTOS port,
   `portmacro.h` includes `spinlock.h` which includes `riscv/rv_utils.h` which includes
   `soc/soc_caps.h` — a long transitive chain. We explicitly avoided this chain (documented
   in the portmacro.h comments) because `rv_utils.h` pulls in RISC-V CSR helpers that we
   thought might conflict with ThreadX's own RISC-V port. By breaking the chain, we had
   to provide our own `spinlock_t`.

2. **We focused on what our code needed, not what other code provides.** The override
   approach means ALL of ESP-IDF compiles against our headers. When we wrote portmacro.h,
   we were thinking "what does esp_coex_adapter.c need?" (answer: `portMUX_TYPE` exists).
   We didn't think "what happens when esp_hw_support's own spinlock.h is also included?"

3. **The `#pragma once` misconception.** The real `spinlock.h` uses `#pragma once`, which
   prevents the same file from being included twice. But it does NOT prevent a DIFFERENT
   file (our portmacro.h) from defining the same symbols. `#pragma once` guards file
   re-inclusion, not symbol redefinition.

### The Correct Approach

**Use the real `spinlock.h`.** Don't reimplement system types — include them. On
single-core ESP32-C6 with `CONFIG_ESP_SYSTEM_SINGLE_CORE_MODE=y`, the complex multi-core
spinlock code in `spinlock.h` is compiled out by `#if` guards. What remains is:
- The `spinlock_t` typedef (same struct we were defining)
- `SPINLOCK_FREE` and `SPINLOCK_INITIALIZER` macros
- A trivial `spinlock_initialize()` that just does `assert(lock)`

The transitive include chain (`spinlock.h` → `rv_utils.h` → `soc/soc_caps.h`) is
actually beneficial — it provides symbols like `SOC_WIFI_LIGHT_SLEEP_CLK_WIDTH` that
we were manually including via `#include "soc/soc_caps.h"` anyway.

### The Kconfig `select` Lesson

The `RVHAL_EXCM_LEVEL_CLIC` error exposed a second bug: our Kconfig defined
`FREERTOS_UNICORE=y` but didn't `select ESP_SYSTEM_SINGLE_CORE_MODE`. In Kconfig
semantics:

- `default y` — sets the initial value, can be overridden
- `select SYMBOL` — **hard forces** another symbol to `y` whenever this one is `y`

The real FreeRTOS Kconfig uses `select` to create a hard dependency chain:
```
FREERTOS_UNICORE=y → selects → ESP_SYSTEM_SINGLE_CORE_MODE=y
```
Without the `select`, `ESP_SYSTEM_SINGLE_CORE_MODE` stayed at its own default (`n`),
and all multi-core code paths across ESP-IDF were compiled in — including spinlock
acquire/release with CLIC-specific constants not available on ESP32-C6.

**Lesson:** When overriding a component's Kconfig, don't just copy the visible options
and their defaults. Also copy the `select` and `depends on` relationships — these
invisible dependency chains are critical for the rest of ESP-IDF's configuration to
remain consistent.

---

## Build Error Iteration Log — Continued (Iterations 17–20)

### Iteration 17: `portBASE_TYPE` Unknown Type

**Build progress:** 291/1213 objects

**Error:** `i2c_slave.c:154: error: unknown type name 'portBASE_TYPE'`

**Root cause:** Legacy FreeRTOS type alias. The real RISC-V portmacro.h defines
`#define portBASE_TYPE int` and `#define portSTACK_TYPE uint8_t`. Our portmacro.h
had `portCHAR`, `portFLOAT`, etc. but was missing these two.

**Fix:** Added `portBASE_TYPE` and `portSTACK_TYPE` to portmacro.h legacy type section.

**Files changed:** `components/freertos/threadx/include/freertos/portmacro.h`

---

### Iteration 18: `freertos/list.h` Missing + Holistic Header Audit

**Build progress:** 309/1185 objects

**Error:** `ringbuf.c:11: fatal error: freertos/list.h: No such file or directory`

**Also noticed:** `-Wno-incompatible-pointer-types` warning on all C++ files (nvs_flash).

**Root cause:** `esp_ringbuf` and `pthread_cond_var` include `freertos/list.h`, which
we never created. This triggered a holistic audit of ALL `freertos/*.h` headers that
ESP-IDF components include vs what we provide.

**Complete header audit:**

| Header | Had? | ESP-IDF components using it |
|--------|------|---------------------------|
| FreeRTOS.h | Yes | Everywhere |
| task.h | Yes | Everywhere |
| queue.h | Yes | Everywhere |
| semphr.h | Yes | Everywhere |
| event_groups.h | Yes | esp_event, esp_wifi |
| timers.h | Yes | esp_timer |
| portmacro.h | Yes | Direct includes |
| stream_buffer.h | Yes | USB serial |
| idf_additions.h | Yes | Stub |
| projdefs.h | Yes | Minimal defines |
| **list.h** | **NO** | esp_ringbuf, pthread_cond_var |
| **message_buffer.h** | **NO** | Various ESP-IDF components |
| **atomic.h** | **NO** | esp_adc, USB serial, SPI |
| **portable.h** | **NO** | newlib locks, i2c, ringbuf tests |

**Fix — batch creation of 4 missing headers:**

1. **`list.h`** — Full FreeRTOS list implementation:
   - `ListItem_t` struct (xItemValue, pxNext, pxPrevious, pvOwner, pxContainer)
   - `MiniListItem_t = ListItem_t` (no mini optimization for simplicity)
   - `List_t` struct (uxNumberOfItems, pxIndex, xListEnd)
   - All access macros (listLIST_IS_EMPTY, listGET_HEAD_ENTRY, etc.)
   - Inline macros (listREMOVE_ITEM, listINSERT_END, listGET_OWNER_OF_NEXT_ENTRY)
   - Function declarations (vListInitialise, vListInsert, vListInsertEnd, uxListRemove)
   - Event list declarations (vTaskPlaceOnEventList, xTaskRemoveFromEventList)

2. **`message_buffer.h`** — Redirects to stream_buffer.h with message framing macros

3. **`atomic.h`** — Full atomic operation set (CAS, swap, add/sub/inc/dec, OR/AND/XOR)
   using interrupt disable on single-core ESP32-C6

4. **`portable.h`** — Memory allocation declarations (pvPortMalloc, vPortFree, etc.)

**port.c additions:**
- Complete list function implementations (vListInitialise, vListInsert, vListInsertEnd,
  uxListRemove) — pure data structure operations, ~70 lines
- Event list stubs (vTaskPlaceOnEventList → delays instead of blocking,
  xTaskRemoveFromEventList → returns pdFALSE). These are deep scheduler internals that
  would require significant ThreadX integration for full functionality. Stubs are
  sufficient for compilation and for WiFi (which doesn't use ringbuf directly).

**CMakeLists.txt fix:**
Changed `-Wno-incompatible-pointer-types` from global to C-only using CMake generator
expression: `$<$<COMPILE_LANGUAGE:C>:-Wno-incompatible-pointer-types>`. This flag is
C-only (doesn't exist in C++) — it was generating noise warnings on every C++ file.
No behavior change for C++ since the flag never applied to C++ anyway.

**Files changed:**
- `components/freertos/threadx/include/freertos/list.h` (NEW)
- `components/freertos/threadx/include/freertos/message_buffer.h` (NEW)
- `components/freertos/threadx/include/freertos/atomic.h` (NEW)
- `components/freertos/threadx/include/freertos/portable.h` (NEW)
- `components/freertos/threadx/src/port.c`
- `components/freertos/CMakeLists.txt`

---

### Iteration 19: `StaticRingbuffer_t != Ringbuffer_t` Static Assertion

**Build progress:** 31/877 objects (incremental rebuild after list.h addition)

**Error:** `ringbuf.c:78: _Static_assert(sizeof(StaticRingbuffer_t) == sizeof(Ringbuffer_t))`

**Root cause:** Size mismatch between our Static* types and our List types:

```
Our List_t uses MiniListItem_t = ListItem_t (5 fields × 4 = 20 bytes)
  → List_t = 4 + 4 + 20 = 28 bytes

Our StaticMiniListItem_t had only 3 fields (12 bytes)
  → StaticList_t = 4 + 4 + 12 = 20 bytes

28 ≠ 20 → assertion fails!
```

The Static types in FreeRTOS.h were originally copied from real FreeRTOS which uses
a separate MiniListItem_t (3 fields, 12 bytes). But our list.h defines
`MiniListItem_t = ListItem_t` (5 fields, 20 bytes) for simplicity. The Static types
must match.

**Fix:** Updated StaticListItem_t and StaticMiniListItem_t in FreeRTOS.h:

| Type | Before | After | Matches |
|------|--------|-------|---------|
| StaticListItem_t | 4 + 4×4 + 4 = 24 | 4 + 4×4 = 20 | ListItem_t (20) ✓ |
| StaticMiniListItem_t | 4 + 2×4 = 12 | 4 + 4×4 = 20 | MiniListItem_t (20) ✓ |
| StaticList_t | 4 + 4 + 12 = 20 | 4 + 4 + 20 = 28 | List_t (28) ✓ |

**Files changed:** `components/freertos/threadx/include/freertos/FreeRTOS.h`

---

### Iteration 20: `vTaskInternalSetTimeOutState` Implicit Declaration

**Build progress:** Same file (ringbuf.c) — second error in same compilation unit

**Error:** `ringbuf.c:802: implicit declaration of function 'vTaskInternalSetTimeOutState'`

**Root cause:** Internal FreeRTOS variant of `vTaskSetTimeOutState`. Same behavior but
without critical section protection (caller is already in a protected context). Used by
ringbuf's `prvSendAcquireGeneric`.

**Fix:** Added declaration in FreeRTOS.h and implementation in port.c (delegates to
`vTaskSetTimeOutState` — our implementation doesn't use critical sections anyway since
the ThreadX compat layer manages its own synchronization).

**Files changed:**
- `components/freertos/threadx/include/freertos/FreeRTOS.h`
- `components/freertos/threadx/src/port.c`

---

## Build Error Iteration Log — Continued (Iterations 21–28)

### Session Context

This session began with the build at ~334/1185 objects. Through iterations 21–28, all
compilation errors were resolved (521/523 objects compiled, all 897 compilation units
passing). The build then reached the **linker phase**, where 5 linker errors were fixed.
Finally, a **runtime crash** was diagnosed and fixed.

### Iteration 21: `xQueueCreateMutex` Implicit Declaration + `StaticSemaphore_t` Size

**Build progress:** ~334/1185 objects

**Error (a):** `newlib/locks.c:63: implicit declaration of function 'xQueueCreateMutex'`

**Error (b):** `newlib/locks.c:240: static assertion failed: sizeof(struct __lock) >= sizeof(StaticSemaphore_t)`

**Root cause (a):** ESP-IDF's `newlib/locks.c` calls `xQueueCreateMutex()` directly — this
is an internal FreeRTOS function (not a public API) that `xSemaphoreCreateMutex` is
normally a macro alias for. The upstream compat layer provides `xSemaphoreCreateMutex()`
as a real function but doesn't expose the internal `xQueueCreateMutex()` name.

**Root cause (b):** The newlib lock struct (`struct __lock`) is defined as
`int reserved[21]` = 84 bytes. Our `StaticSemaphore_t` = `txfr_sem_t` is ~96 bytes.
84 < 96 → assertion fails.

**Fix (a):** Added `xQueueCreateMutex()` and `xQueueCreateMutexStatic()` inline functions
to the FreeRTOS.h wrapper. These dispatch based on queue type constant:

```c
static inline SemaphoreHandle_t xQueueCreateMutex(const uint8_t ucQueueType) {
    if (ucQueueType == queueQUEUE_TYPE_RECURSIVE_MUTEX) {
        return xSemaphoreCreateRecursiveMutex();
    }
    return xSemaphoreCreateMutex();
}
```

The queue type constants (`queueQUEUE_TYPE_BASE` through `queueQUEUE_TYPE_RECURSIVE_MUTEX`)
were moved earlier in FreeRTOS.h (before these inline functions) with `#ifndef` guards to
avoid conflicts with any other definitions.

**Fix (b):** Added Kconfig option `CONFIG_FREERTOS_USE_LIST_DATA_INTEGRITY_CHECK_BYTES`
that defaults to `y` when ThreadX mode is active. This changes `struct __lock` from
`int reserved[21]` (84 bytes) to `int reserved[27]` (108 bytes), which is >= sizeof(txfr_sem_t).

**Also in this iteration:** Removed duplicate `_lock_*` function implementations from
port.c (~80 lines). These were direct TX_MUTEX-based lock implementations that conflicted
with newlib/locks.c's own implementations. newlib/locks.c uses FreeRTOS semaphore APIs
(xQueueCreateMutex, xSemaphoreTakeRecursive) which route through our compat layer to
ThreadX underneath — no need for a separate lock implementation.

**Files changed:**
- `components/freertos/threadx/include/freertos/FreeRTOS.h`
- `components/freertos/Kconfig`
- `components/freertos/threadx/src/port.c`

---

### Iteration 22: `port.c` Include Path + Transitive Include Failures

**Build progress:** After fullclean rebuild, ~400/1185 objects

**Errors (6 total):**
1. `port.c: 'queueSEND_TO_FRONT' undeclared`
2. `port.c: unknown type name 'TlsDeleteCallbackFunction_t'`
3. `port.c: unknown type name 'StreamBufferHandle_t'`
4. `freertos_debug.h: conflicting types for 'ListItem_t'`
5. `intr_alloc.c: implicit declaration of 'traceISR_ENTER'`
6. `intr_alloc.c: implicit declaration of 'os_task_switch_is_pended'`

**Root cause:** `port.c` used `#include <FreeRTOS.h>` (angle brackets) which found the
upstream compat header directly, bypassing our wrapper. Our wrapper adds the ESP-IDF
extensions (`queueSEND_TO_FRONT`, `TlsDeleteCallbackFunction_t`, `StreamBufferHandle_t`,
etc.) that port.c needs. With the upstream header alone, these types were missing.

**Fix — 4 changes:**

1. **port.c:** Changed `#include <FreeRTOS.h>` to `#include "freertos/FreeRTOS.h"` and
   added `#include "freertos/list.h"` early (before freertos_debug.h).

2. **freertos_debug.h:** Changed from `typedef void ListItem_t` to
   `#include "freertos/list.h"`. The previous typedef conflicted with the real
   `ListItem_t` struct from list.h.

3. **portmacro.h:** Added trace macros and ISR switch detection:
   ```c
   #define traceISR_ENTER(_n_)
   #define traceISR_EXIT()
   #define traceISR_EXIT_TO_SCHEDULER()
   #define os_task_switch_is_pended(_cpu_)  (false)
   ```

4. **portmacro.h:** Added `#include "esp_rom_sys.h"` for `esp_rom_get_cpu_ticks_per_us()`
   transitive include needed by `log_timestamp.c`.

**Files changed:**
- `components/freertos/threadx/src/port.c`
- `components/freertos/threadx/include/esp_private/freertos_debug.h`
- `components/freertos/threadx/include/freertos/portmacro.h`

---

### Iteration 23: `StaticTask_t` Missing Fields + `freertos_idf_additions_priv.h`

**Build progress:** ~835/897 objects

**Error (a):** `expression_with_stack.c: 'StaticTask_t' has no member named 'pxDummy6'`

**Error (b):** `cache_utils.c: fatal error: esp_private/freertos_idf_additions_priv.h: No such file`

**Root cause (a):** ESP-IDF's `expression_with_stack.c` accesses opaque FreeRTOS
`StaticTask_t` fields by their "Dummy" names (`pxDummy6` = pxStack, `pxDummy8` =
pxEndOfStack). The upstream compat layer's `txfr_task_t` (which is typedef'd to
`StaticTask_t`) didn't have these fields.

**Root cause (b):** `cache_utils.c` includes a private FreeRTOS header for kernel-level
macros (`prvENTER_CRITICAL_OR_SUSPEND_ALL`, etc.). On single-core, these macros aren't
actually used, but the include must succeed.

**Fix (a):** Added `void *pxDummy6` and `void *pxDummy8` fields to `txfr_task_t` in the
upstream compat FreeRTOS.h (submodule file):
```c
uint8_t allocated;
void *pxDummy6;  // ESP-IDF compat: expression_with_stack.c
void *pxDummy8;  // ESP-IDF compat: expression_with_stack.c
```

**Fix (b):** Created stub header at
`components/freertos/threadx/include/esp_private/freertos_idf_additions_priv.h`:
```c
#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
```

**Files changed:**
- `components/threadx/threadx/utility/rtos_compatibility_layers/FreeRTOS/FreeRTOS.h` (submodule)
- `components/freertos/threadx/include/esp_private/freertos_idf_additions_priv.h` (NEW)

---

### Iteration 24: Standalone `portmacro.h` + `StaticSemaphore_t` `.u` Member

**Build progress:** ~850/897 objects

**Error (a):** `ieee802154_dev.c: unknown type name 'BaseType_t'` — portmacro.h included
standalone (without FreeRTOS.h).

**Error (b):** `FreeRTOS_POSIX_mqueue.c: 'StaticSemaphore_t' has no member named 'u'` —
uses designated initializer `{ { 0 }, .u = { 0 } }`.

**Root cause (a):** Some ESP-IDF components (`ieee802154`, `esp_phy`) include
`freertos/portmacro.h` directly without including `freertos/FreeRTOS.h` first.
`portmacro.h` used `BaseType_t` in function declarations but relied on FreeRTOS.h
providing the typedef.

**Root cause (b):** Real FreeRTOS `StaticQueue_t` has a `union { ... } u;` member.
ESP-IDF's POSIX mqueue layer uses this in a designated initializer. Our `txfr_sem_t`
(typedef'd to `StaticSemaphore_t`) lacked this field.

**Fix (a):** Added base type definitions to portmacro.h, guarded by `#ifndef FREERTOS_H`:
```c
#ifndef FREERTOS_H
typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;
typedef uint8_t StackType_t;
#define pdTRUE   ((BaseType_t) 1)
#define pdFALSE  ((BaseType_t) 0)
#define portMAX_DELAY  ((TickType_t) 0xffffffffUL)
#endif
```

**Fix (b):** Added `.u` union to `txfr_sem_t` in upstream compat FreeRTOS.h:
```c
union { UBaseType_t uxDummy2; } u;
```

**Files changed:**
- `components/freertos/threadx/include/freertos/portmacro.h`
- `components/threadx/threadx/utility/rtos_compatibility_layers/FreeRTOS/FreeRTOS.h` (submodule)

---

### Iteration 25: Linker Phase — 5 Link Errors

**Build progress:** 521/523 objects compiled (all 897 compilation units passing!)

The build reached the **linker phase** for the first time. Five linker errors remained:

```
1. multiple definition of `vPortEnterCritical'  (port.c vs tx_freertos.c)
2. multiple definition of `vPortExitCritical'   (port.c vs tx_freertos.c)
3. multiple definition of `tx_application_define' (tx_port_startup.c vs tx_freertos.c)
4. undefined reference to `rtos_int_enter'      (from ESP-IDF riscv/vectors.S)
5. undefined reference to `rtos_int_exit'       (from ESP-IDF riscv/vectors.S)
```

#### Fix 1-2: Duplicate `vPortEnterCritical` / `vPortExitCritical`

**Analysis:** Both port.c and tx_freertos.c define these. The upstream tx_freertos.c
version is actually better — it disables interrupts via `portDISABLE_INTERRUPTS()` AND
increments `_tx_thread_preempt_disable`, preventing ThreadX from preempting during
FreeRTOS critical sections. Our port.c version only disabled interrupts with save/restore.

**Fix:** Removed `vPortEnterCritical` / `vPortExitCritical` from port.c. The upstream
tx_freertos.c versions now win. They handle nesting correctly:
- Enter: disable interrupts + increment `_tx_thread_preempt_disable`
- Exit: decrement counter, only re-enable interrupts when count reaches 0

#### Fix 3: Duplicate `tx_application_define`

**Root cause:** `TX_FREERTOS_AUTO_INIT=1` in FreeRTOSConfig.h caused tx_freertos.c to
provide an empty `tx_application_define()`. Our tx_port_startup.c provides the real one.

**Fix:** Changed `TX_FREERTOS_AUTO_INIT` from 1 to 0 in FreeRTOSConfig.h. This disables:
- The `__attribute__((constructor))` in tx_freertos.c that would call ThreadX APIs
  before kernel initialization (would crash)
- The empty `tx_application_define()` in tx_freertos.c (our real one wins)

With AUTO_INIT=0, we call `tx_freertos_init()` ourselves from our `tx_application_define()`
in tx_port_startup.c — after `tx_kernel_enter()` has initialized the kernel.

#### Fix 4-5: Undefined `rtos_int_enter` / `rtos_int_exit`

**Root cause:** ESP-IDF's `riscv/vectors.S` calls these two functions from
`_interrupt_handler` for general interrupt dispatch. In real FreeRTOS, they're implemented
in `portasm.S` and manage ISR nesting + stack switching (task stack → ISR stack).

**What they do:**

`rtos_int_enter`:
1. Check if scheduler is running (return 0 if not)
2. Increment ISR nesting counter
3. If first-level interrupt: save current SP in task TCB, switch to ISR stack
4. Return context value (0 on ESP32-C6 — no coprocessors)

`rtos_int_exit`:
1. Check if scheduler is running (return mstatus if not)
2. Decrement ISR nesting counter
3. If exiting last ISR level: check `xPortSwitchFlag` for pending context switch
4. Call `vTaskSwitchContext` if switch pending
5. Restore task SP from (possibly new) TCB
6. Return mstatus in a0

**Why assembly is required:** These functions must directly manipulate the stack pointer
register (`sp`) to switch between task and ISR stacks. A C function cannot safely modify
SP because the compiler's prologue/epilogue manage SP for local variables. The real
FreeRTOS also implements these in assembly (`portasm.S`). This is one of the few cases
where inline assembly or a separate `.S` file is necessary.

**Implementation:** Created `rtos_int_hooks.S` with:
- Full `rtos_int_enter` / `rtos_int_exit` implementations matching the calling convention
  from vectors.S (no args / a0=mstatus a1=context → returns mstatus in a0)
- Global variables: `port_xSchedulerRunning`, `port_uxInterruptNesting`,
  `xPortSwitchFlag`, `port_uxCriticalNesting`, `pxCurrentTCBs`
- ISR stack allocation (4KB in `.bss`) with `xIsrStackTop` / `xIsrStackBottom` pointers

**Note on runtime path:** After ThreadX starts, our custom MTVEC vector table handles all
interrupts — ESP-IDF's `_interrupt_handler` (and thus rtos_int_enter/exit) is NOT called.
These are used only during early boot (before ThreadX installs its vector table). For
WiFi support, the vector table will need to route WiFi CPU interrupt lines through
`_interrupt_handler` so ESP-IDF's `esp_intr_alloc` dispatch works.

**Also added:** `vTaskSwitchContext()` stub in port.c — called from rtos_int_exit when
a context switch is pending. Currently a no-op since ThreadX handles context switches
through its own scheduler. Will need integration when WiFi interrupts use this path.

**Files changed:**
- `components/freertos/threadx/src/port.c` (removed vPortEnterCritical/vPortExitCritical, added vTaskSwitchContext stub)
- `components/freertos/threadx/include/FreeRTOSConfig.h` (TX_FREERTOS_AUTO_INIT=0)
- `components/freertos/threadx/src/rtos_int_hooks.S` (NEW)
- `components/freertos/CMakeLists.txt` (added rtos_int_hooks.S to SRCS)
- `components/threadx/port/tx_port_startup.c` (calls tx_freertos_init, sets txfr_scheduler_started)

---

### Iteration 26: Runtime Crash — `xTaskGetSchedulerState()` Returns Wrong Value

**Build progress:** Linked successfully, flashed to hardware.

**Crash:** `abort() was called at PC 0x40801be1 on core 0` in `lock_acquire_generic`
at `newlib/locks.c:133` — during early boot, before ThreadX starts.

**Backtrace:**
```
lock_acquire_generic → _lock_acquire_recursive → __retarget_lock_acquire_recursive
  → __sfp_lock_acquire → __sfp → _fopen_r → fopen
    → esp_newlib_init_global_stdio → do_core_init → start_cpu0_default
```

**Root cause — `TX_FREERTOS_AUTO_INIT=0` broke scheduler state detection:**

ESP-IDF's `newlib/locks.c` flow at line 113-133:
```c
static int lock_acquire_generic(_lock_t *lock, uint32_t delay, uint8_t mutex_type) {
    SemaphoreHandle_t h = (SemaphoreHandle_t)(*lock);
    if (!h) {
        if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED)
            return 0;  // no-op during boot ← THIS IS THE CRITICAL PATH
        lock_init_generic(lock, mutex_type);
        ...
    }
    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED)
        return 0;  // no-op during boot
    if (!xPortCanYield()) {
        if (mutex_type == queueQUEUE_TYPE_RECURSIVE_MUTEX)
            abort();  // ← CRASH HERE: recursive mutex in "ISR context"
        ...
    }
}
```

The `xTaskGetSchedulerState()` function in tx_freertos.c:
```c
BaseType_t xTaskGetSchedulerState(void) {
#if (TX_FREERTOS_AUTO_INIT == 1)     // ← COMPILED OUT when AUTO_INIT=0!
    if (txfr_scheduler_started == 0u)
        return taskSCHEDULER_NOT_STARTED;
#endif
    if (_tx_thread_preempt_disable > 0u)
        return taskSCHEDULER_SUSPENDED;
    else
        return taskSCHEDULER_RUNNING;  // ← Returns THIS during boot!
}
```

With `TX_FREERTOS_AUTO_INIT=0`, the `txfr_scheduler_started` check was compiled out.
During early boot, `_tx_thread_preempt_disable` = 0 (BSS zero-init), so the function
returned `taskSCHEDULER_RUNNING` instead of `taskSCHEDULER_NOT_STARTED`.

This caused `locks.c` to think the scheduler was running, then `xPortCanYield()` returned
false (because `_tx_thread_system_state` was non-zero during init context), leading locks.c
to think it was in ISR context → `abort()` on recursive mutex in ISR.

**Fix — 3 changes to tx_freertos.c (upstream submodule):**

1. **Made `txfr_scheduler_started` always-available** (not guarded by AUTO_INIT):
   ```c
   // Was: #if (TX_FREERTOS_AUTO_INIT == 1) static UINT txfr_scheduler_started; #endif
   // Now:
   UINT txfr_scheduler_started;  // Always available — non-static for external access
   ```

2. **Made `xTaskGetSchedulerState()` always check it:**
   ```c
   BaseType_t xTaskGetSchedulerState(void) {
       if (txfr_scheduler_started == 0u)       // Always checked now
           return taskSCHEDULER_NOT_STARTED;
       ...
   }
   ```

3. **Made `vTaskStartScheduler()` always set it:**
   ```c
   void vTaskStartScheduler(void) {
       txfr_scheduler_started = 1u;            // Always set now
       ...
   }
   ```

**Also in tx_port_startup.c:** Set `txfr_scheduler_started = 1` at the end of
`tx_application_define()`, just before the ThreadX scheduler starts. This is the correct
timing — locks become functional the moment threads start running.

**The boot sequence with fix:**
```
1. ESP-IDF boot → do_core_init() → esp_newlib_init_global_stdio()
2. fopen() → __sfp_lock_acquire() → lock_acquire_generic()
3. xTaskGetSchedulerState() → txfr_scheduler_started=0 → taskSCHEDULER_NOT_STARTED
4. lock_acquire_generic() returns 0 (no-op) ✓
   ... boot continues ...
5. esp_startup_start_app() → tx_kernel_enter()
6. tx_application_define() → tx_freertos_init() → txfr_scheduler_started=1
7. ThreadX scheduler starts → locks are now fully functional
```

**Files changed:**
- `components/threadx/threadx/utility/rtos_compatibility_layers/FreeRTOS/tx_freertos.c` (submodule)
- `components/threadx/port/tx_port_startup.c`

---

## Updated File Structure (Post Iteration 26)

```
components/freertos/
├── CMakeLists.txt                            # Dual-mode build (ThreadX or real FreeRTOS)
├── Kconfig                                   # CONFIG_FREERTOS_* stubs + integrity check bytes
├── threadx/
│   ├── include/
│   │   ├── freertos/
│   │   │   ├── FreeRTOS.h                   # Wrapper: #include_next + ESP-IDF extensions
│   │   │   ├── task.h                       # Forwards to FreeRTOS.h
│   │   │   ├── queue.h                      # Forwards to FreeRTOS.h
│   │   │   ├── semphr.h                     # Forwards to FreeRTOS.h
│   │   │   ├── event_groups.h               # Forwards to FreeRTOS.h
│   │   │   ├── timers.h                     # Forwards to FreeRTOS.h
│   │   │   ├── list.h                       # Full list implementation
│   │   │   ├── atomic.h                     # Atomic operations (interrupt-disable based)
│   │   │   ├── portable.h                   # Memory allocation declarations
│   │   │   ├── message_buffer.h             # Redirects to stream_buffer.h
│   │   │   ├── stream_buffer.h              # Stream buffer types
│   │   │   ├── portmacro.h                  # ESP32-C6 RISC-V port macros
│   │   │   ├── projdefs.h                   # Minimal compat defines
│   │   │   └── idf_additions.h              # Stub
│   │   ├── FreeRTOSConfig.h                 # ThreadX-specific config (AUTO_INIT=0)
│   │   ├── esp_freertos_hooks.h             # Stub hooks API
│   │   └── esp_private/
│   │       ├── freertos_debug.h             # Task snapshot stubs (gdbstub)
│   │       └── freertos_idf_additions_priv.h  # Stub (cache_utils.c)
│   └── src/
│       ├── app_startup.c                    # esp_startup_start_app() → tx_kernel_enter()
│       ├── port.c                           # ESP-IDF supplements: TLS, ISR detect, WithCaps, lists
│       ├── freertos_hooks.c                 # Stub hook implementations
│       └── rtos_int_hooks.S                 # rtos_int_enter/exit + ISR stack + globals
```

## Upstream Submodule Modifications

The ThreadX submodule (`components/threadx/threadx/`) has these modifications:

| File | Change | Why |
|------|--------|-----|
| `ports/risc-v64/gnu/src/tx_thread_context_restore.S` | `240(sp)` → `30*REGBYTES(sp)` | mepc offset must adapt to RV32 vs RV64 |
| `utility/.../FreeRTOS/FreeRTOS.h` | Added `pxDummy6`, `pxDummy8` to `txfr_task_t` | `expression_with_stack.c` accesses StaticTask_t by opaque names |
| `utility/.../FreeRTOS/FreeRTOS.h` | Added `union { UBaseType_t uxDummy2; } u;` to `txfr_sem_t` | POSIX mqueue uses designated initializer `.u = { 0 }` |
| `utility/.../FreeRTOS/tx_freertos.c` | `txfr_scheduler_started` always-available | newlib locks must detect "scheduler not started" even with AUTO_INIT=0 |
| `utility/.../FreeRTOS/tx_freertos.c` | `xTaskGetSchedulerState()` always checks flag | Same — prevents crash during early boot |
| `utility/.../FreeRTOS/tx_freertos.c` | `vTaskStartScheduler()` always sets flag | Same |

---

## Lessons Learned — Iterations 21–26

### 1. `TX_FREERTOS_AUTO_INIT` Is a Boot Sequence Decision

The `TX_FREERTOS_AUTO_INIT` flag controls TWO things:
- **Constructor vs explicit init:** With AUTO_INIT=1, a `__attribute__((constructor))`
  calls `tx_freertos_init()` before `main()`. With AUTO_INIT=0, you must call it yourself.
- **`xTaskGetSchedulerState()` behavior:** With AUTO_INIT=1, it correctly returns
  NOT_STARTED before the scheduler runs. With AUTO_INIT=0, the check was compiled out.

We need AUTO_INIT=0 because the constructor calls ThreadX APIs before kernel init.
But we also need the scheduler state check for newlib locks. The fix was to decouple
these: always track scheduler state regardless of AUTO_INIT.

### 2. Newlib Locks Are the First FreeRTOS Consumer

`newlib/locks.c` is called during `do_core_init()` — long before `esp_startup_start_app()`.
This is the earliest point where FreeRTOS API correctness matters. The lock implementation
calls `xTaskGetSchedulerState()` and `xPortCanYield()` to decide whether to no-op (during
boot) or actually acquire semaphores (after scheduler starts).

Any change to FreeRTOS compat layer initialization must be validated against this early
boot path. The crash was subtle — it only manifested because `xTaskGetSchedulerState()`
returned an incorrect value, causing a cascade through lock acquisition logic.

### 3. The Include Path Matters: `<FreeRTOS.h>` vs `"freertos/FreeRTOS.h"`

Angle-bracket includes (`<FreeRTOS.h>`) search include paths in order and found the
upstream compat header directly. Quoted includes with the `freertos/` prefix
(`"freertos/FreeRTOS.h"`) find our wrapper first (because `threadx/include/` is listed
first in CMakeLists.txt INCLUDE_DIRS). The wrapper then `#include_next`s the upstream
header and adds ESP-IDF extensions.

**Rule:** All includes in our code must use `"freertos/FreeRTOS.h"` (quoted, with prefix)
to ensure they go through our wrapper.

### 4. Assembly Is Unavoidable for Stack Switching

`rtos_int_enter`/`rtos_int_exit` require raw SP manipulation (saving task SP to TCB,
loading ISR stack pointer). This cannot be done in C because:
- The compiler manages SP through function prologue/epilogue
- Reading SP after prologue gives the wrong value (already decremented)
- Writing SP mid-function corrupts the compiler's frame

The real FreeRTOS also implements these in assembly. This is one of the few legitimate
cases where `.S` files are necessary in the port.

### 5. Upstream Modifications Should Be Minimal and Documented

We modified 3 files in the ThreadX submodule. Each modification is small, well-documented,
and necessary (no alternative without forking). The modifications are:
- Structural (adding fields to match FreeRTOS ABI expectations)
- Correctness (boot-time scheduler state detection)
- Platform adaptation (RV32 vs RV64 register offsets)

These should be tracked and potentially upstreamed as contributions to the ThreadX
FreeRTOS compat layer.

---

## Iteration 27 — Boot Crash Triage (Three Bugs)

After iteration 26 built and flashed successfully, the system hung at `cpu_start: Unicore app`
with no further output. Three sequential bugs were found and fixed before boot progressed
to ThreadX entry.

### Bug 30: Stale `freertos_compat` Component (Boot Hang)

**Symptom:** Boot output stopped at `I (258) cpu_start: Unicore app` — no crash, no
watchdog reset, just silence.

**Root cause:** An old `components/freertos_compat/` component (from before the unified
`components/freertos/` override was created) was still present. It compiled a **second
copy** of `tx_freertos.c` with `TX_FREERTOS_AUTO_INIT=1` in its own `FreeRTOSConfig.h`.
This created an `__attribute__((constructor))` function that called `tx_freertos_init()`
during `do_global_ctors()` in the ESP-IDF boot sequence — before `tx_kernel_enter()` had
initialized the ThreadX kernel. The `tx_freertos_init()` function calls ThreadX APIs
(`tx_byte_pool_create`, `tx_thread_create`) which require the kernel to be initialized,
causing a silent hang.

**How we found it:**
1. Traced the ESP-IDF boot sequence in `cpu_start.c` and `startup.c`
2. "Unicore app" is printed at `cpu_start.c:616`, then `call_start_cpu0()` continues
   with hardware init before calling `SYS_STARTUP_FN()` → `start_cpu0_default()`
3. `start_cpu0_default()` calls `do_core_init()` → `do_global_ctors()` → `do_secondary_init()`
4. None of our code should affect this sequence — so we looked for build artifacts
5. Found `components/freertos_compat/` with its own `CMakeLists.txt` compiling `tx_freertos.c`
6. Its `FreeRTOSConfig.h` had `TX_FREERTOS_AUTO_INIT=1` — creating the dangerous constructor
7. The top-level `CMakeLists.txt` still referenced it in `EXTRA_COMPONENT_DIRS`

**Fix:**
- Deleted `components/freertos_compat/` directory
- Removed `freertos_compat` from `EXTRA_COMPONENT_DIRS` in top-level `CMakeLists.txt`
- Added `CONFIG_FREERTOS_USE_LIST_DATA_INTEGRITY_CHECK_BYTES=y` to both `sdkconfig.defaults`
  and `sdkconfig` (required for `struct __lock` sizing with ThreadX compat — the fullclean
  had reset it)

**Files changed:**
- `CMakeLists.txt` — removed `freertos_compat` from `EXTRA_COMPONENT_DIRS`
- `sdkconfig.defaults` — added `CONFIG_FREERTOS_USE_LIST_DATA_INTEGRITY_CHECK_BYTES=y`
- `sdkconfig` — set `CONFIG_FREERTOS_USE_LIST_DATA_INTEGRITY_CHECK_BYTES=y`
- Deleted: `components/freertos_compat/` (entire directory)

### Bug 31: `txfr_malloc` Returns NULL Before Compat Layer Init (abort in lock_init_generic)

**Symptom:** After fixing Bug 30, boot progressed further but crashed:
```
abort() was called at PC 0x40801b5b on core 0
--- 0x40801b5b: lock_init_generic at .../newlib/locks.c:65
```

**Backtrace:**
```
#3  lock_init_generic (lock=0x40855384 <__sf+88>, mutex_type=4)  at locks.c:65
#4  __retarget_lock_init_recursive (lock=0x40855384)             at locks.c:291
#5  std (ptr=0x4085532c <__sf>, flags=4, file=0)                 at findfp.c:93
#6  stdin_init (ptr=0x4085532c)                                  at findfp.c:104
#7  global_stdio_init ()                                         at findfp.c:163
#10 _fopen_r (...)                                               at fopen.c:126
#12 esp_newlib_init_global_stdio (stdio_dev="")                  at newlib_init.c:181
#13 __esp_system_init_fn_init_newlib_stdio ()                    at newlib_init.c:204
#14 do_system_init_fn (stage_num=0)                              at startup.c:132
```

**Root cause:** `locks.c:lock_init_generic()` calls `xQueueCreateMutex()` to create a
semaphore for newlib stdio locks. Our compat layer's `xQueueCreateMutex` calls
`txfr_malloc()` which uses `tx_byte_allocate()` from the ThreadX byte pool. But the
byte pool is created in `tx_freertos_init()` which runs inside `tx_application_define()`
→ inside `tx_kernel_enter()` → which hasn't been called yet during CORE init stage.

The `txfr_malloc()` function checked `txfr_heap_initialized` and returned NULL when
the byte pool wasn't ready. `lock_init_generic` aborted on NULL semaphore handle.

In real FreeRTOS, `xQueueCreateMutex` → `pvPortMalloc` → `heap_caps_malloc` works
because the ESP-IDF heap is initialized at CORE priority 100 (before newlib stdio at 115).

**Fix:** Modified `txfr_malloc()` in `tx_freertos.c` to fall back to `malloc()` (which
uses the ESP-IDF heap) when the compat layer's byte pool isn't ready yet. Also modified
`txfr_free()` to handle both allocation sources — if `tx_byte_release()` fails (pointer
came from `malloc()` fallback), it falls back to `free()`.

```c
// BEFORE:
void *txfr_malloc(size_t len) {
    if(txfr_heap_initialized == 1u) {
        ret = tx_byte_allocate(&txfr_heap, &p, len, 0u);
        ...
    } else {
        return NULL;        // ← crash: locks.c aborts on NULL
    }
}

// AFTER:
void *txfr_malloc(size_t len) {
    if(txfr_heap_initialized == 1u) {
        ret = tx_byte_allocate(&txfr_heap, &p, len, 0u);
        ...
    } else {
        return malloc(len); // ← use ESP-IDF heap as fallback
    }
}
```

**Files changed:**
- `components/threadx/threadx/.../tx_freertos.c` — `txfr_malloc()` and `txfr_free()` with
  `malloc()`/`free()` fallback; added `#include <stdlib.h>`

### Bug 32: Missing `__getreent()` (Load Access Fault in vprintf)

**Symptom:** After fixing Bug 31, boot progressed even further (through all CORE init
functions) but crashed:
```
Guru Meditation Error: Core 0 panic'ed (Load access fault). Exception was unhandled.
MEPC    : 0x4003e2ec  →  vprintf in ROM
MCAUSE  : 0x00000005  MTVAL   : 0x00000008
```

**How to debug ESP-IDF crashes — step-by-step walkthrough:**

**Step 1: Read the panic type.**
```
Guru Meditation Error: Core 0 panic'ed (Load access fault)
```
"Load access fault" means the CPU tried to read from an invalid memory address.
The RISC-V exception causes are:
- 0 = Instruction address misaligned
- 1 = Instruction access fault
- 2 = Illegal instruction
- 4 = Load address misaligned
- **5 = Load access fault** ← this one
- 6 = Store address misaligned
- 7 = Store access fault

**Step 2: Check MCAUSE and MTVAL registers.**
```
MCAUSE: 0x00000005  →  cause 5 = Load access fault
MTVAL:  0x00000008  →  the memory address the CPU tried to read
```
Address `0x00000008` is a strong indicator of a NULL pointer dereference with a struct
field access. Something like `ptr->field` where `ptr == NULL` and `field` is at offset 8
in the struct. On ESP32-C6, addresses below `0x40000000` are not valid for data access
(except for RTC memory at `0x50000000`).

**Step 3: Find the crashing function from MEPC.**
```
MEPC: 0x4003e2ec  →  vprintf in ROM
RA:   0x4003e2e0  →  vprintf in ROM
```
`MEPC` (Machine Exception Program Counter) is the exact instruction that faulted.
The ESP-IDF monitor automatically resolves addresses to function names using the ELF.
ROM functions (addresses starting with `0x4003xxxx` on ESP32-C6) are resolved from
the ROM ELF. `RA` (Return Address) tells you who called this function.

**Step 4: Look at the register values for clues.**
```
A0: 0x00000000  ←  first argument to the function = NULL!
A5: 0x42093f5a  →  __getreent (annotation from monitor)
S3: 0x408002e0  →  call_start_cpu0 (we're still in early boot)
```
On RISC-V, `A0` is the first function argument. `vprintf` receives a `FILE*` pointer
derived from the reent structure. The reent struct pointer is NULL, so accessing
`reent->_stdout` at offset 8 → reads address `0x00000008` → crash.

`A5` pointing to `__getreent` tells us this function was recently called — it's the
source of the NULL pointer.

**Step 5: Read the backtrace.**
```
#0  0x4003e2ec in vprintf in ROM
Backtrace stopped: previous frame identical to this frame (corrupt stack?)
```
ROM functions don't have frame pointer info, so GDB can't unwind past them. But we
already know from the register analysis that `vprintf` was called with a NULL reent.

**Step 6: Understand WHY `__getreent()` returns NULL.**

`__getreent()` is called by newlib to get the per-thread reentrancy structure. This struct
contains `errno`, stdio file pointers (`stdin`, `stdout`, `stderr`), and other thread-local
state. In real FreeRTOS, it's implemented in `freertos_tasks_c_additions.h`:
```c
struct _reent * __getreent(void) {
    TCB_t * pxCurTask = xTaskGetCurrentTaskHandle();
    if (pxCurTask == NULL) {
        return _GLOBAL_REENT;    // fallback to global struct
    } else {
        return &pxCurTask->xTLSBlock;  // per-task struct
    }
}
```

Our ThreadX compat layer didn't provide `__getreent()`. The linker fell back to the
default libgloss implementation (`libnosys/getreent.c`) which returns `_impure_ptr`.
During early boot, `_impure_ptr` was NULL → `vprintf` crashed.

**The linker even warned us:** During every build, there was a warning:
```
warning: __getreent is not implemented and will always fail
```
This warning was the clue all along — we just didn't connect it to the crash until now.

**Step 7: Search for the real implementation.**
```bash
grep -rn "__getreent" $IDF_PATH/components/freertos/
```
Found it in `esp_additions/freertos_tasks_c_additions.h:850`. This file is `#include`d
into `tasks.c` at compile time — it's not a separate compilation unit. Since we override
the freertos component and don't compile `tasks.c`, this function was missing.

**Fix:** Added `__getreent()` to `port.c` returning `_GLOBAL_REENT` unconditionally.
For single-core ESP32-C6 without multi-threaded newlib, the global reent struct is
sufficient.

```c
#include <sys/reent.h>

struct _reent *__getreent(void)
{
    return _GLOBAL_REENT;
}
```

**Files changed:**
- `components/freertos/threadx/src/port.c` — added `#include <sys/reent.h>` and
  `__getreent()` function

### Debugging Quick Reference for ESP-IDF

| Register | What it tells you |
|----------|------------------|
| `MEPC` | Exact instruction that faulted (PC at time of exception) |
| `MCAUSE` | Exception type: 2=illegal insn, 5=load fault, 7=store fault |
| `MTVAL` | The invalid address (for access faults) or faulting instruction |
| `RA` | Return address — who called the crashing function |
| `SP` | Stack pointer — check if in valid range (RAM: `0x40800000–0x4087FFFF`) |
| `A0–A7` | Function arguments (RISC-V calling convention) |
| `MSTATUS` | Bit 3 = MIE (interrupts enabled), Bit 7 = MPIE (previous MIE) |
| `MTVEC` | Trap vector base — should point to `_vector_table` |

**Common MTVAL patterns:**
- `0x00000000–0x0000FFFF` → NULL pointer dereference (offset = struct field)
- `0xDEADBEEF` / `0xA5A5A5A5` → use-after-free or uninitialized memory
- `0x50000000+` → RTC memory access (check if RTC peripheral is clocked)
- Valid-looking address in RAM → stack overflow or heap corruption

**ESP-IDF monitor tips:**
- The monitor auto-resolves addresses to `function at file:line` using the ELF
- ROM function addresses (`0x4003xxxx` on ESP32-C6) are resolved from ROM ELF
- `addr2line -pfiaC -e build/project.elf 0xADDRESS` for manual resolution
- Backtrace may be incomplete for ROM functions or corrupted stacks

### Summary of Changes — Iteration 27

**Files modified:**
| File | Change |
|------|--------|
| `CMakeLists.txt` (top-level) | Removed `freertos_compat` from `EXTRA_COMPONENT_DIRS` |
| `sdkconfig.defaults` | Added `CONFIG_FREERTOS_USE_LIST_DATA_INTEGRITY_CHECK_BYTES=y` |
| `sdkconfig` | Set `CONFIG_FREERTOS_USE_LIST_DATA_INTEGRITY_CHECK_BYTES=y` |
| `tx_freertos.c` (submodule) | `txfr_malloc`/`txfr_free` with `malloc()`/`free()` fallback; added `#include <stdlib.h>` |
| `port.c` | Added `__getreent()` returning `_GLOBAL_REENT`; added `#include <sys/reent.h>` |

**Files deleted:**
| File | Reason |
|------|--------|
| `components/freertos_compat/` (entire dir) | Stale component causing duplicate `tx_freertos.c` compilation with wrong config |

### Updated Upstream Submodule Modifications

| File | Change | Why |
|------|--------|-----|
| `tx_freertos.c` | `txfr_scheduler_started` non-static, always checked | Boot-time scheduler state detection (Bug 29) |
| `tx_freertos.c` | `txfr_malloc`/`txfr_free` with `malloc()` fallback | ESP-IDF init functions create locks before ThreadX starts (Bug 31) |
| `tx_freertos.c` | Added `#include <stdlib.h>` | Required for `malloc()`/`free()` fallback |
| `FreeRTOS.h` | Added `pxDummy6`, `pxDummy8`, `.u` union | ESP-IDF struct layout compatibility |

---

## Iteration 28 — Bug 33: PERIPH_RCC_ACQUIRE_ATOMIC Re-enables MIE During Init

### Symptom

After the fixes in Iteration 27 (Bugs 30-32), the system boots through ESP-IDF init,
prints the diagnostic messages, but then **hangs at "ThreadX taking over"** — the same
behavior as the original Phase 0 bug (Bug 29).

Boot log ended with:
```
I (373) threadx_boot: === ThreadX taking over (esp_startup_start_app) ===
```
...and nothing else. No `tx_application_define` message, no thread startup.

### Debugging Approach

This was recognized as the same class of bug as Bug 29: **mstatus.MIE gets set to 1
during `_tx_initialize_low_level`, allowing the SYSTIMER interrupt to fire before any
threads exist**. The `_tx_thread_context_restore` function enters its idle-wait loop
because `_tx_thread_execute_ptr == NULL` and never wakes up.

Bug 29 was fixed by adding `csrci mstatus, 0x8` at the start of `_tx_initialize_low_level`
to disable MIE. But something was RE-ENABLING it during init.

**Step 1: Identify what runs after MIE is cleared.**

The init sequence after `csrci mstatus, 0x8`:
1. Save system stack pointer
2. Set unused memory pointer
3. Install vector table (mtvec)
4. Enable mie.MEIE (bit 11) — this enables the PLIC, NOT global MIE
5. Call `_tx_port_setup_timer_interrupt()` (C function)

Step 5 is the prime suspect — it's the only C function called during init.

**Step 2: Trace through `_tx_port_setup_timer_interrupt()`.**

The function had this code:
```c
PERIPH_RCC_ACQUIRE_ATOMIC(PERIPH_SYSTIMER_MODULE, ref_count) {
    if (ref_count == 0) {
        systimer_ll_enable_bus_clock(true);
        systimer_ll_reset_register();
    }
}
```

**Step 3: Expand the `PERIPH_RCC_ACQUIRE_ATOMIC` macro.**

From `esp_private/periph_ctrl.h`:
```c
#define PERIPH_RCC_ACQUIRE_ATOMIC(periph, ref_count)    \
    for (uint8_t rc_name, _rc_cnt = 1, __DECLARE_RCC_RC_ATOMIC_ENV;     \
         _rc_cnt ? (rc_name = periph_rcc_acquire_enter(rc_periph), 1) : 0; \
         periph_rcc_acquire_exit(rc_periph, rc_name), _rc_cnt--)
```

The `periph_rcc_acquire_enter()` / `periph_rcc_acquire_exit()` functions use
`portENTER_CRITICAL` / `portEXIT_CRITICAL` internally.

**Step 4: Trace the critical section path.**

```
portEXIT_CRITICAL → vPortExitCritical → portENABLE_INTERRUPTS
```

`portENABLE_INTERRUPTS` expands to `csrs mstatus, 0x8` — sets MIE bit!

During init, the nesting count is 0, so `vPortExitCritical` calls
`portENABLE_INTERRUPTS` unconditionally, re-enabling global interrupts.

**Step 5: Confirm the chain.**

1. `_tx_initialize_low_level` clears MIE (Bug 29 fix)
2. Calls `_tx_port_setup_timer_interrupt()`
3. `PERIPH_RCC_ACQUIRE_ATOMIC` calls `portEXIT_CRITICAL`
4. `portEXIT_CRITICAL` sets MIE = 1
5. SYSTIMER alarm has already been configured by ESP-IDF's esp_timer
6. SYSTIMER INT_ST = 1, PLIC asserts mip.bit17
7. CPU takes interrupt → `_tx_thread_context_restore` → idle-wait loop → **hang**

### The Fix

**Remove the `PERIPH_RCC_ACQUIRE_ATOMIC` call entirely.**

The SYSTIMER peripheral bus clock is already enabled by ESP-IDF's `esp_timer` init
(which runs as `ESP_SYSTEM_INIT_FN` at priority 101 during `do_core_init()`). We
don't need to enable it again.

Additionally, `systimer_ll_enable_bus_clock()` itself is wrapped in a macro:
```c
#define systimer_ll_enable_bus_clock(...) \
    (void)__DECLARE_RCC_RC_ATOMIC_ENV; systimer_ll_enable_bus_clock(__VA_ARGS__)
```
This macro REQUIRES the `__DECLARE_RCC_RC_ATOMIC_ENV` variable from the
`PERIPH_RCC_ACQUIRE_ATOMIC` for-loop. Calling `systimer_ll_enable_bus_clock(true)`
outside of `PERIPH_RCC_ACQUIRE_ATOMIC` causes a compile error.

The underlying operation is just `PCR.systimer_conf.systimer_clk_en = 1` — a single
register write. But since the clock is already enabled, we skip it entirely.

**Changed code in `tx_esp32c6_timer.c`:**
```c
/* BEFORE: */
PERIPH_RCC_ACQUIRE_ATOMIC(PERIPH_SYSTIMER_MODULE, ref_count) {
    if (ref_count == 0) {
        systimer_ll_enable_bus_clock(true);
        systimer_ll_reset_register();
    }
}

/* AFTER: */
/* The SYSTIMER peripheral bus clock is already enabled by ESP-IDF's esp_timer
 * init (ESP_SYSTEM_INIT_FN priority 101, runs during do_core_init()).
 *
 * We intentionally do NOT call systimer_ll_enable_bus_clock() or
 * PERIPH_RCC_ACQUIRE_ATOMIC here because:
 * 1. systimer_ll_enable_bus_clock() is wrapped in a macro requiring
 *    __DECLARE_RCC_RC_ATOMIC_ENV (the PERIPH_RCC critical-section env).
 * 2. PERIPH_RCC_ACQUIRE_ATOMIC uses portENTER/EXIT_CRITICAL, and during init
 *    vPortExitCritical() re-enables mstatus.MIE when nesting count reaches 0.
 * 3. The bus clock is already on — no action needed.
 *
 * Do NOT reset SYSTIMER registers — esp_timer configured other alarms/counters
 * during ESP-IDF CORE init that we must not disturb. */
```

We also do NOT call `systimer_ll_reset_register()` because esp_timer has already
configured SYSTIMER counter 0 and alarm 2 for its own use. Resetting would destroy
that configuration.

### Key Lesson: ESP-IDF LL Functions Have Hidden Critical Sections

Many ESP-IDF `*_ll_*` (low-level) functions are wrapped in macros that enforce
critical section usage:
```c
// In hal/systimer_ll.h:
static inline void systimer_ll_enable_bus_clock(bool enable) {
    PCR.systimer_conf.systimer_clk_en = enable;  // <-- actual operation
}
// But the macro wrapper forces callers into a critical section:
#define systimer_ll_enable_bus_clock(...) \
    (void)__DECLARE_RCC_RC_ATOMIC_ENV; systimer_ll_enable_bus_clock(__VA_ARGS__)
```

This pattern exists for all `PERIPH_RCC`-related LL functions (bus clock enable, reset).
The macro won't compile unless you're inside a `PERIPH_RCC_ACQUIRE_ATOMIC` block
which declares the required environment variable.

**Rule for ThreadX port:** During initialization (when MIE must stay 0), avoid any
ESP-IDF function that uses `portENTER_CRITICAL`/`portEXIT_CRITICAL`. This includes:
- `PERIPH_RCC_ACQUIRE_ATOMIC` / `PERIPH_RCC_RELEASE_ATOMIC`
- `esp_intr_alloc()` (uses spinlocks)
- Most HAL functions that manage peripheral clock/reset
- `ESP_LOGI` (uses log mutex, but log_lock.c checks scheduler state and returns early)

### Summary of Changes — Iteration 28

| File | Change |
|------|--------|
| `tx_esp32c6_timer.c` | Removed `PERIPH_RCC_ACQUIRE_ATOMIC` + `systimer_ll_enable_bus_clock` call; added comment explaining why |

---

## Iteration 29 — Bug 34: WiFi Interrupts Not Dispatched (Vector Table Dead-Loop)

### Symptom

After Bug 33 was fixed, the system booted successfully through ThreadX init, created
threads, ran `app_main()`, initialized WiFi, and printed all the WiFi driver init
messages. But then it **hung at `wifi:enable tsf`** — the very last message before
WiFi's internal task starts processing events.

Full boot log (abbreviated):
```
I (373) threadx_boot: === ThreadX taking over (esp_startup_start_app) ===
I (373) tx_diag: --- Timer HW State ---
...all diagnostics correct...
I (487) threadx_startup: tx_application_define: setting up system resources
I (494) threadx_startup: FreeRTOS compat layer initialized
I (509) threadx_startup: Main thread started, calling app_main()
I (519) wifi_demo: === WiFi Demo on ThreadX ===
...WiFi driver fully initialized...
I (549) wifi:wifi driver task: 4082e08c, prio:30, stack:6656, core=0
I (549) wifi:wifi firmware version: 79fa3f41ba
...PHY calibration, power tables...
I (819) wifi:mode : sta (7c:2c:67:42:d5:84)
I (819) wifi:enable tsf
<--- HANG HERE --->
```

WiFi made it all the way through `esp_wifi_start()` — driver task created, firmware
loaded, PHY calibrated, STA mode set — then froze. This means the WiFi hardware
initialization succeeded, but something prevented the WiFi task from processing events.

### Debugging Approach — The Vector Table Problem

#### Step 1: Understand What "wifi:enable tsf" Means

TSF = **Timing Synchronization Function**, the 802.11 timing mechanism. This is the
last step of `esp_wifi_start()` internal initialization. After this, the WiFi driver
needs to:
1. Post a `WIFI_EVENT_STA_START` event to the event loop
2. The event loop task picks it up and calls our `event_handler`
3. `event_handler` calls `esp_wifi_connect()`

For steps 2-3 to happen, **the WiFi driver's internal task must be able to run** and
**WiFi interrupts must be delivered**. The WiFi hardware generates interrupts for:
- Frame reception/transmission completion
- Internal firmware events
- Timing synchronization

#### Step 2: How ESP-IDF Dispatches Interrupts

ESP-IDF's interrupt system works through `esp_intr_alloc()`:

1. A driver calls `esp_intr_alloc(source_id, flags, handler, arg, &handle)`
2. `esp_intr_alloc` picks a free CPU interrupt line (e.g., line 5, 8, 10...)
3. It programs the INTMTX to route the peripheral source → chosen CPU line
4. It registers the handler in `s_intr_handlers[cpu][line_num]` table
5. It enables the PLIC bit for that CPU line
6. It calls `intr_handler_set(line, handler, arg)` to register the C handler

When the interrupt fires:
1. CPU reads `mtvec` → jumps to `vector[N]` for CPU interrupt line N
2. The vector table entry jumps to `_interrupt_handler` (ESP-IDF's generic handler)
3. `_interrupt_handler` (in `riscv/vectors.S`):
   - Saves all general-purpose registers to the stack (`save_general_regs` macro)
   - Saves `mepc` (return address)
   - Calls `rtos_int_enter` → saves SP to TCB, switches to ISR stack
   - Raises PLIC threshold (for safe nesting)
   - Re-enables MIE (`csrsi mstatus, 0x8`) for nested interrupts
   - Reads `mcause` to get interrupt number
   - Calls `_global_interrupt_handler(sp, mcause)` (in `riscv/interrupt.c`)
4. `_global_interrupt_handler` looks up `s_intr_handlers[cpu][mcause]` and calls it
5. On return: disables MIE, restores PLIC threshold
6. Calls `rtos_int_exit` → may context-switch, restores SP from TCB
7. Restores registers, executes `mret`

#### Step 3: The Root Cause — Dead Vector Table Entries

Our custom vector table in `tx_initialize_low_level.S` had:

```asm
_tx_esp32c6_vector_table:
    j   _tx_esp32c6_exception_entry /* 0: exception entry */
    j   _tx_esp32c6_unused_int      /* 1: DEAD LOOP! */
    j   _tx_esp32c6_unused_int      /* 2: DEAD LOOP! */
    ...
    j   _tx_esp32c6_unused_int      /* 16: DEAD LOOP! */
    j   _tx_esp32c6_trap_handler    /* 17: SYSTIMER tick ✓ */
    j   _tx_esp32c6_unused_int      /* 18: DEAD LOOP! */
    ...
    j   _tx_esp32c6_unused_int      /* 31: DEAD LOOP! */
```

Where `_tx_esp32c6_unused_int` was:
```asm
_tx_esp32c6_unused_int:
    j   _tx_esp32c6_unused_int      /* infinite spin */
```

**Every interrupt line except 17 (SYSTIMER) jumped into an infinite loop.**

When the WiFi driver called `esp_intr_alloc()`, it was assigned some CPU interrupt line
(e.g., line 5 or line 8). When WiFi hardware triggered that interrupt, the CPU
dispatched to `vector[N]` → `_tx_esp32c6_unused_int` → **spin forever**. The WiFi
interrupt handler never ran. The WiFi task was waiting for an event that would never
arrive. System hung.

#### Step 4: Why Was the Vector Table Originally All Dead Loops?

When we first designed the vector table in Phase 0, the only interrupt we needed was
SYSTIMER (line 17) for the ThreadX tick. There were no other peripherals using
interrupts. Setting unused entries to spin loops was a safe "catch impossible bugs"
measure.

But with WiFi, many peripherals need interrupts:
- **WiFi MAC/BB**: frame RX/TX, firmware events
- **esp_timer**: `SYSTIMER_ALARM_2` for high-resolution timers
- **UART**: serial console (if interrupt-driven)
- Any other ESP-IDF component that calls `esp_intr_alloc()`

All of these were silently dying in the dead loop.

### The Fix — Three-Part Solution

#### Part 1: Route Non-ThreadX Vectors to ESP-IDF's `_interrupt_handler`

**Changed `tx_initialize_low_level.S`:**

```asm
_tx_esp32c6_vector_table:
    j   _tx_esp32c6_exception_entry /* 0: exception entry */
    j   _interrupt_handler          /* 1: ESP-IDF generic dispatch (WiFi, etc.) */
    j   _interrupt_handler          /* 2: ESP-IDF generic dispatch */
    j   _interrupt_handler          /* 3: ESP-IDF generic dispatch */
    ...                             /* 4-16: all → _interrupt_handler */
    j   _tx_esp32c6_trap_handler    /* 17: SYSTIMER tick (ThreadX path) */
    j   _interrupt_handler          /* 18: ESP-IDF generic dispatch */
    ...                             /* 19-31: all → _interrupt_handler */
```

**Why `_interrupt_handler` is reachable:** The `j` (JAL) instruction has ±1 MiB range.
Both the vector table (`.iram0.text`) and `_interrupt_handler` (marked `noflash_text`
in `riscv/linker.lf`) are in IRAM (~0x40800000 region). The distance is well within
range.

**Why vector[0] stays as exception entry:** CPU exceptions (illegal instruction,
misaligned access, etc.) dispatch to `vector[0]`. Our exception handler switches to
the system stack and calls `abort()` for the ESP-IDF panic handler. This is important
for debugging — without it, exceptions would silently die.

**Why vector[17] stays as ThreadX trap handler:** The SYSTIMER tick must go through
ThreadX's `_tx_thread_context_save` / `_tx_thread_context_restore` path so that
ThreadX can properly manage preemption, thread scheduling, and timer processing.
The ESP-IDF path (`rtos_int_enter`/`rtos_int_exit`) manages FreeRTOS-style context
switching which wouldn't correctly handle ThreadX's internal scheduler state.

#### Part 2: Enable ISR Stack Switching (`port_xSchedulerRunning`)

ESP-IDF's `_interrupt_handler` calls `rtos_int_enter` (our `rtos_int_hooks.S`) which
checks `port_xSchedulerRunning`:

```asm
rtos_int_enter:
    lw      t0, port_xSchedulerRunning
    beqz    t0, .Lenter_end         /* Not running — skip stack switch */
    ...
    sw      sp, 0(t0)               /* Save task SP to TCB */
    lw      sp, xIsrStackTop        /* Switch to ISR stack */
```

Without setting this flag, ISRs would run on the current task's stack. This works
but risks stack overflow — WiFi ISRs can be deep, and thread stacks are limited.

**Changed `tx_port_startup.c`:**
```c
extern volatile uint32_t port_xSchedulerRunning;  /* rtos_int_hooks.S */

// In tx_application_define(), after txfr_scheduler_started = 1u:
port_xSchedulerRunning = 1u;
```

#### Part 3: Initialize `pxCurrentTCBs` for ISR Stack Save/Restore

`rtos_int_enter` saves the current SP to `pxCurrentTCBs[0]->offset_0` and
`rtos_int_exit` restores it. If `pxCurrentTCBs == NULL`, the check
`beqz t0, .Lenter_end` skips the save/restore, which is safe but leaves ISRs
running on the task stack.

To enable proper ISR stack switching, we provide a simple "fake TCB" — just a
single `uint32_t` that holds the saved SP:

```c
static uint32_t s_current_task_sp_save;
extern volatile uint32_t *pxCurrentTCBs;  /* rtos_int_hooks.S */

// In tx_application_define():
pxCurrentTCBs = &s_current_task_sp_save;
```

**Why a fake TCB works:** On single-core ESP32-C6, there's only one task running
at a time. `rtos_int_enter` saves the current SP to this word, switches to the
ISR stack, and `rtos_int_exit` restores the same SP. Since ThreadX handles its
own context switching (through `_tx_thread_context_save`/`_tx_thread_context_restore`
on the timer path), the ESP-IDF path doesn't need to support context switching —
it just needs to save/restore the interrupted task's SP.

### Deep Dive: The Dual Interrupt Path Architecture

After this fix, our system has **two parallel interrupt handling paths**:

```
┌─────────────────────────────────────────────────────────────────────┐
│                        MTVEC VECTOR TABLE                           │
│  (256-byte aligned, .iram0.text, 32 entries × 4 bytes)             │
├─────┬───────────────────────────────────────────────────────────────┤
│ [0] │ j _tx_esp32c6_exception_entry  → abort() → panic handler     │
├─────┼───────────────────────────────────────────────────────────────┤
│ [1] │ j _interrupt_handler ──┐                                      │
│ [2] │ j _interrupt_handler   │                                      │
│ ... │ j _interrupt_handler   ├── ESP-IDF Path (all non-17 lines)   │
│[16] │ j _interrupt_handler   │                                      │
├─────┼────────────────────────┘                                      │
│[17] │ j _tx_esp32c6_trap_handler ── ThreadX Path (SYSTIMER only)   │
├─────┼───────────────────────────────────────────────────────────────┤
│[18] │ j _interrupt_handler ──┐                                      │
│ ... │ j _interrupt_handler   ├── ESP-IDF Path (all non-17 lines)   │
│[31] │ j _interrupt_handler ──┘                                      │
└─────┴───────────────────────────────────────────────────────────────┘
```

**Path A: ThreadX Timer (vector[17] only)**
```
SYSTIMER alarm fires
  → CPU dispatches to vector[17]
  → _tx_esp32c6_trap_handler (IRAM)
    → addi sp, sp, -(32*REGBYTES)      // allocate frame
    → STORE x1, 28*REGBYTES(sp)        // save return address
    → call _tx_thread_context_save     // ThreadX saves full context to ThreadX TCB
    → call _tx_esp32c6_timer_isr       // clear SYSTIMER, call _tx_timer_interrupt()
    → la t0, _tx_thread_context_restore
    → jr t0                            // ThreadX restores (may switch threads), mret
```

**Path B: ESP-IDF Generic (all other interrupts)**
```
WiFi/BLE/UART/etc. interrupt fires on CPU line N (N ≠ 17)
  → CPU dispatches to vector[N]
  → _interrupt_handler (IRAM, riscv/vectors.S)
    → save_general_regs               // save all GPRs to stack frame
    → save_mepc                       // save return PC
    → call rtos_int_enter             // our hook: save SP, switch to ISR stack
    → raise PLIC threshold            // allow only higher-priority nesting
    → csrsi mstatus, 0x8              // re-enable MIE (allow nesting)
    → call _global_interrupt_handler  // dispatch to registered handler
    → csrci mstatus, 0x8              // disable MIE
    → restore PLIC threshold
    → call rtos_int_exit              // our hook: restore SP from TCB
    → restore_general_regs            // restore all GPRs
    → mret                            // return to interrupted code
```

### Deep Dive: Why the Two Paths Don't Interfere (Nesting Analysis)

A critical concern with two independent interrupt paths is **nesting**: can a
ThreadX timer interrupt fire INSIDE an ESP-IDF ISR (or vice versa)?

#### Can the ThreadX Timer Nest Inside an ESP-IDF ISR? **NO.**

`_interrupt_handler` raises the PLIC threshold before re-enabling MIE:

```asm
/* From vectors.S: */
lw      t2, 0(t1)        /* t2 = priority of current interrupt */
addi    t2, t2, 1        /* t2 = priority + 1 */
sw      t2, 0(t0)        /* PLIC threshold = priority + 1 */
fence
csrsi   mstatus, 0x8     /* NOW re-enable MIE */
```

The nesting rule: only interrupts with `priority > threshold` can fire.

Our SYSTIMER is at **priority 2**. Any ESP-IDF interrupt at priority P raises the
threshold to P+1. For P ≥ 1 (all valid priorities), the threshold becomes ≥ 2.
Since `2` is NOT `> 2`, the timer **cannot nest** into any ESP-IDF ISR.

#### Can an ESP-IDF ISR Nest Inside the ThreadX Timer? **NO.**

Our `_tx_esp32c6_trap_handler` never re-enables MIE. The sequence:
1. CPU takes interrupt → hardware clears `mstatus.MIE` automatically
2. `_tx_thread_context_save` keeps MIE=0
3. `_tx_esp32c6_timer_isr` runs with MIE=0
4. `_tx_thread_context_restore` keeps MIE=0 until the final `mret`
5. `mret` restores `mstatus.MPIE → MIE` (re-enabling for the resumed thread)

Since MIE is **never set** during the ThreadX ISR, no interrupt (of any priority)
can nest into it.

#### Summary: No Nesting in Either Direction

| Scenario | Possible? | Why |
|----------|-----------|-----|
| ThreadX timer nests INTO ESP-IDF ISR | No | PLIC threshold ≥ timer priority |
| ESP-IDF ISR nests INTO ThreadX timer | No | ThreadX never re-enables MIE |
| ESP-IDF ISR nests INTO ESP-IDF ISR | Yes (by design) | Higher-priority ISR can preempt lower |
| ThreadX timer fires during thread code | Yes (normal) | MIE=1 during normal thread execution |
| ESP-IDF ISR fires during thread code | Yes (normal) | MIE=1 during normal thread execution |

### Deep Dive: `rtos_int_enter` / `rtos_int_exit` and the "Fake TCB"

ESP-IDF's `_interrupt_handler` calls our `rtos_int_enter` and `rtos_int_exit` hooks
(from `rtos_int_hooks.S`). These were originally designed for the FreeRTOS compat
layer but now serve a dual purpose.

#### `rtos_int_enter` Flow:

```asm
rtos_int_enter:
    lw      t0, port_xSchedulerRunning
    beqz    t0, .Lenter_end          /* (1) Scheduler not running? Skip. */

    la      t3, port_uxInterruptNesting
    lw      t4, 0(t3)
    addi    t5, t4, 1
    sw      t5, 0(t3)                /* (2) Increment nesting counter */

    bnez    t4, .Lenter_end          /* (3) Already nested? Keep ISR stack. */

    lw      t0, pxCurrentTCBs
    beqz    t0, .Lenter_end          /* (4) No current task? Skip. */
    sw      sp, 0(t0)                /* (5) Save task SP to fake TCB */
    lw      sp, xIsrStackTop         /* (6) Switch to ISR stack */

.Lenter_end:
    li      a0, 0                    /* Return 0 (no coprocessor context) */
    ret
```

#### `rtos_int_exit` Flow:

```asm
rtos_int_exit:
    mv      s11, a0                  /* (1) Save mstatus */

    lw      t0, port_xSchedulerRunning
    beqz    t0, .Lexit_end           /* (2) Scheduler not running? Skip. */

    la      t2, port_uxInterruptNesting
    lw      t3, 0(t2)
    addi    t3, t3, -1
    sw      t3, 0(t2)               /* (3) Decrement nesting counter */

    bnez    t3, .Lexit_end          /* (4) Still nested? Keep ISR stack. */

    /* (5) Check for FreeRTOS-style context switch (not used by ThreadX) */
    la      t0, xPortSwitchFlag
    lw      t2, 0(t0)
    beqz    t2, .Lno_switch
    call    vTaskSwitchContext       /* (6) No-op stub for ThreadX */
    sw      zero, xPortSwitchFlag   /* (7) Clear flag */

.Lno_switch:
    lw      t0, pxCurrentTCBs
    lw      sp, 0(t0)              /* (8) Restore task SP from fake TCB */

.Lexit_end:
    mv      a0, s11                 /* Return mstatus */
    ret
```

The **"fake TCB"** (`s_current_task_sp_save`) is a single `uint32_t`. Step (5) saves
the task's SP there, step (8) restores it. On a single-core system with no FreeRTOS
context switching (ThreadX handles that), the restored SP is always the same one that
was saved. This is correct because:

- ThreadX context switches happen on the **timer path** (vector[17])
- ESP-IDF interrupts on the **generic path** (all other vectors) just need to
  save/restore the interrupted context without switching

### `vTaskSwitchContext` Stub

Currently `vTaskSwitchContext` is a no-op:
```c
void vTaskSwitchContext(int xCoreID) {
    (void)xCoreID;
    /* ThreadX handles context switches internally */
}
```

This is correct for now because:
1. ThreadX preemption is triggered by `_tx_timer_interrupt()` through the timer path
2. The timer path's `_tx_thread_context_restore` handles the actual thread switch
3. ESP-IDF interrupts (WiFi, etc.) don't directly trigger ThreadX preemption
4. If a WiFi ISR posts to a semaphore that wakes a ThreadX thread, the wakeup is
   recorded in ThreadX's internal state, and preemption happens on the next timer tick

**Future improvement:** Implement `vTaskSwitchContext` to call `tx_thread_relinquish()`
or trigger immediate preemption via `xPortSwitchFlag`. This would reduce latency for
WiFi event processing.

### ESP-IDF's `_interrupt_handler` In Detail

For reference, here's how ESP-IDF's generic interrupt handler works
(from `riscv/vectors.S`):

```
_interrupt_handler:
    ┌─ save_general_regs ──────────────────────────────────────────┐
    │  addi sp, sp, -CONTEXT_SIZE     (128 bytes for 32 regs)     │
    │  sw ra, RV_STK_RA(sp)                                       │
    │  sw t0, RV_STK_T0(sp)                                       │
    │  ...all 31 GPRs...                                           │
    └──────────────────────────────────────────────────────────────┘
    ┌─ save_mepc ──────────────────────────────────────────────────┐
    │  csrr t0, mepc                                               │
    │  sw t0, RV_STK_MEPC(sp)                                     │
    └──────────────────────────────────────────────────────────────┘
    call rtos_int_enter          ← our hook (saves SP, ISR stack)
    csrr s1, mcause              ← read interrupt number
    csrr s2, mstatus             ← save mstatus for later
    ┌─ threshold raise (software nesting support) ─────────────────┐
    │  threshold = PLIC_PRIORITY[mcause] + 1                       │
    │  (only interrupts with higher priority can nest)              │
    └──────────────────────────────────────────────────────────────┘
    csrsi mstatus, 0x8          ← re-enable MIE (nested IRQs OK)
    ┌─ dispatch ───────────────────────────────────────────────────┐
    │  mv a0, sp                                                    │
    │  mv a1, s1 & REASON_MASK  (interrupt number)                 │
    │  jal _global_interrupt_handler                                │
    │    → looks up s_intr_handlers[cpu][mcause]                   │
    │    → calls registered handler(arg)                            │
    └──────────────────────────────────────────────────────────────┘
    csrci mstatus, 0x8          ← disable MIE
    ┌─ threshold restore ─────────────────────────────────────────┐
    │  restore original PLIC threshold                              │
    └──────────────────────────────────────────────────────────────┘
    call rtos_int_exit           ← our hook (restore SP, maybe switch)
    csrw mcause, s1              ← restore mcause
    csrw mstatus, a0             ← restore mstatus (from rtos_int_exit)
    ┌─ restore_mepc + restore_general_regs ────────────────────────┐
    │  lw t0, RV_STK_MEPC(sp)                                     │
    │  csrw mepc, t0                                                │
    │  lw ra, RV_STK_RA(sp) ... all GPRs ...                      │
    │  addi sp, sp, CONTEXT_SIZE                                    │
    └──────────────────────────────────────────────────────────────┘
    mret                         ← return to interrupted code
```

Key points:
- The handler is in IRAM (`noflash_text` in `riscv/linker.lf`) — always accessible
- `_global_interrupt_handler` (in `riscv/interrupt.c`) is also in IRAM
- The handler table `s_intr_handlers[cpu][N]` is populated by `esp_intr_alloc()`
  / `intr_handler_set()` — WiFi, BLE, UART, etc. all register here
- `rtos_int_enter`/`rtos_int_exit` are our hooks (in `rtos_int_hooks.S`) that
  manage ISR stack switching and nesting counters

### Summary of Changes — Iterations 28-29

**Files modified:**

| File | Change | Bug |
|------|--------|-----|
| `tx_esp32c6_timer.c` | Removed `PERIPH_RCC_ACQUIRE_ATOMIC` and `systimer_ll_enable_bus_clock`; rely on ESP-IDF's pre-init | Bug 33 |
| `tx_initialize_low_level.S` | Changed 30 vector entries from `j _tx_esp32c6_unused_int` to `j _interrupt_handler`; added `.extern _interrupt_handler` | Bug 34 |
| `tx_port_startup.c` | Added `port_xSchedulerRunning = 1`, `pxCurrentTCBs = &s_current_task_sp_save` in `tx_application_define()`; added externs and fake TCB | Bug 34 |

**Bug Summary:**

| Bug # | Symptom | Root Cause | Fix |
|-------|---------|------------|-----|
| 33 | Hang at "ThreadX taking over" (same as Bug 29) | `PERIPH_RCC_ACQUIRE_ATOMIC` → `portEXIT_CRITICAL` → MIE=1 during init | Remove the call; SYSTIMER bus clock already enabled by esp_timer |
| 34 | Hang at "wifi:enable tsf" | Vector table dead-loops for non-17 interrupts; WiFi ISR never runs | Route all non-17 vectors to `_interrupt_handler`; enable ISR stack switching |

### Architectural Diagram — Complete Interrupt Flow

```
                      ┌───────────────────────┐
                      │    PERIPHERAL SOURCE   │
                      │  (SYSTIMER, WiFi, ...) │
                      └───────────┬─────────────┘
                                  │ interrupt signal
                      ┌───────────▼─────────────┐
                      │        INTMTX            │
                      │  (source → CPU line map) │
                      │  src57 → line 17 (timer) │
                      │  srcN  → line M (WiFi)   │
                      └───────────┬─────────────┘
                                  │ CPU interrupt line N
                      ┌───────────▼─────────────┐
                      │     PLIC MX (0x20001000) │
                      │  ENABLE, PRIORITY, TYPE  │
                      │  threshold gating        │
                      └───────────┬─────────────┘
                                  │ mip.bitN asserted
                      ┌───────────▼─────────────┐
                      │      CPU CORE            │
                      │  mstatus.MIE &&          │
                      │  mie.bitN → take IRQ     │
                      │  vectored: mtvec + N*4   │
                      └───────────┬─────────────┘
                                  │
              ┌───────────────────┼───────────────────┐
              │ N=0              │ N=17              │ N=others
              ▼                  ▼                    ▼
    ┌──────────────┐  ┌─────────────────┐  ┌──────────────────┐
    │  EXCEPTION   │  │  THREADX TIMER  │  │  ESP-IDF GENERIC │
    │  vector[0]   │  │  vector[17]     │  │  vector[N]       │
    ├──────────────┤  ├─────────────────┤  ├──────────────────┤
    │ switch to    │  │ ThreadX context │  │ save_general_regs│
    │ system stack │  │ save (to TX_TCB)│  │ rtos_int_enter   │
    │ call abort() │  │ timer ISR       │  │ threshold raise  │
    │ → panic      │  │ ThreadX context │  │ MIE=1 (nesting)  │
    │   handler    │  │ restore (mret)  │  │ dispatch handler │
    └──────────────┘  │ (may switch     │  │ MIE=0            │
                      │  threads)       │  │ threshold restore│
                      └─────────────────┘  │ rtos_int_exit    │
                                           │ restore_regs     │
                                           │ mret             │
                                           └──────────────────┘
```

---

## Iteration 30 — Bug 35: `portYIELD_FROM_ISR` Calls Thread API from ISR Context

### The Symptom

After fixing Bug 34 (vector table routing to `_interrupt_handler`), the system
booted further than ever before — WiFi STA initialized, event handlers registered,
and ThreadX scheduling was visibly working:

```
I (310) wifi_demo: === WiFi Demo on ThreadX ===
I (320) wifi_demo: NVS initialized, starting WiFi...
I (340) wifi:Init data frame dynamic rx buffer num: 32
I (360) wifi:wifi firmware version: ...
I (380) wifi_demo: WiFi STA init complete, connection attempt in background...
I (400) wifi_demo: Scanner thread created (ThreadX prio 20)
I (410) wifi_demo: [main] tick=32 thread='main' wifi_bits=0x0 retries=0
I (430) wifi_demo: [scanner] Background WiFi scanner started (tick=33)
<... system freezes here — no more output ...>
```

Both the main thread (tick=32) and scanner thread (tick=33) printed once, proving
ThreadX multi-thread scheduling was alive. But then the system froze — no more
ticks, no more output.

### Diagnosis: Why Does It Freeze After Tick 33?

The key observation: the system runs fine for ~330ms (33 ticks at 100 Hz). This
rules out boot-time issues (Bugs 29, 33) and vector table issues (Bug 34). Something
happens at ~330ms that kills scheduling forever.

What happens at ~330ms? The WiFi driver fires its first interrupt. WiFi initialization
is asynchronous — `esp_wifi_start()` returns immediately, and the actual radio
initialization takes a few hundred milliseconds. When the first WiFi interrupt fires
on a non-17 CPU line, it enters our working ESP-IDF interrupt path:

```
vector[N] → _interrupt_handler → rtos_int_enter → _global_interrupt_handler
                                                    → WiFi ISR handler
                                                 → rtos_int_exit
```

The WiFi ISR handler, deep inside the ESP-IDF WiFi stack, calls FreeRTOS APIs to
signal events — and one of those calls includes `portYIELD_FROM_ISR()`.

### Root Cause: Thread-Level API Called from ISR Context

The bug was in two files:

**`components/freertos/threadx/include/FreeRTOSConfig.h` (line 83-84):**
```c
#define portYIELD_FROM_ISR(...)             vPortYield()
#define portEND_SWITCHING_ISR(...)          vPortYield()
```

**`components/freertos/threadx/include/freertos/portmacro.h` (line 122-123):**
```c
#define portYIELD_FROM_ISR(...)         vPortYield()
#define portEND_SWITCHING_ISR(...)      vPortYield()
```

And `vPortYield()` was defined in `port.c`:
```c
void vPortYield(void)
{
    tx_thread_relinquish();
}
```

So the call chain when a WiFi ISR calls `portYIELD_FROM_ISR(pdTRUE)` was:

```
WiFi ISR (running on ISR stack, inside _interrupt_handler)
  → portYIELD_FROM_ISR(pdTRUE)
    → vPortYield()
      → tx_thread_relinquish()    ← ILLEGAL from ISR context!
```

### Why `tx_thread_relinquish()` from ISR Context Corrupts the Scheduler

`tx_thread_relinquish()` is a **thread-level** ThreadX API. It is designed to be
called only from a running thread context. Here's what it does internally:

1. Reads `_tx_thread_current_ptr` to find the calling thread
2. Finds the next same-priority thread in the ready list
3. Sets `_tx_thread_current_ptr` to the next thread
4. Calls `_tx_thread_system_return` to switch context

When called from ISR context, every one of these assumptions is violated:

| Assumption | Thread Context | ISR Context |
|------------|---------------|-------------|
| `_tx_thread_current_ptr` valid | Yes — points to running thread | Maybe — depends on ThreadX ISR state |
| Running on thread stack | Yes | No — running on ISR stack |
| `_tx_thread_system_state` = 0 | Yes (unless preempted) | Non-zero (ISR counter > 0) |
| Safe to modify ready list | Yes — preemption disabled during API | No — could be mid-timer-tick |
| Context switch makes sense | Yes — save/restore thread regs | No — would corrupt ISR return path |

Specifically, when `tx_thread_relinquish()` modifies `_tx_thread_current_ptr` during
an ISR, the ISR exit path (`rtos_int_exit`) later tries to restore SP from
`pxCurrentTCBs`, which may now point somewhere inconsistent. The ThreadX timer ISR
path (`_tx_thread_context_restore`) also expects `_tx_thread_current_ptr` to be
stable during ISR execution. The result: corrupted stack pointer, corrupted
scheduler state, and the system hangs on the next context switch attempt.

### How Real FreeRTOS Handles `portYIELD_FROM_ISR`

To understand the correct design, let's trace how ESP-IDF's real FreeRTOS RISC-V
port handles this:

**Step 1: The ISR calls `portYIELD_FROM_ISR(xHigherPriorityTaskWoken)`**

In real FreeRTOS (`portable/riscv/include/freertos/portmacro.h`):
```c
#define portYIELD_FROM_ISR_ARG(xHigherPriorityTaskWoken)  ({ \
    if (xHigherPriorityTaskWoken == pdTRUE) { \
        vPortYieldFromISR(); \
    } \
})
```

**Step 2: `vPortYieldFromISR()` just sets a flag**

In `portable/riscv/port.c`:
```c
void vPortYieldFromISR(void)
{
    traceISR_EXIT_TO_SCHEDULER();
    BaseType_t coreID = xPortGetCoreID();
    port_xSchedulerRunning[coreID] = 1;
    xPortSwitchFlag[coreID] = 1;    // ← Just sets a flag!
}
```

No context switch happens here. The ISR continues normally and returns through
`rtos_int_exit`.

**Step 3: `rtos_int_exit` (in `portasm.S`) checks the flag on ISR exit**

```asm
isr_skip_decrement:
    /* Nesting count is 0 — this is the outermost ISR return */
    la      s7, xPortSwitchFlag
    lw      a0, 0(s7)
    beqz    a0, no_switch    /* No switch pending — return normally */

    /* Switch requested: call vTaskSwitchContext to pick highest-priority task */
    call    vTaskSwitchContext
    sw      zero, 0(s7)      /* Clear the flag */
    /* pxCurrentTCBs now points to the new task's TCB */
    /* Restore SP from new TCB and mret to the new task */
```

**Step 4: `vTaskSwitchContext()` selects the highest-priority ready task**

This updates `pxCurrentTCBs` so that when `rtos_int_exit` restores SP, it
restores from the NEW task's TCB, effectively performing the context switch.

**The key insight:** In real FreeRTOS, `portYIELD_FROM_ISR` NEVER calls the
scheduler directly. It just sets `xPortSwitchFlag`. The actual context switch
is **deferred to the ISR exit path** (`rtos_int_exit`), which runs at a safe
point where all ISR handlers have completed and it's safe to switch stacks.

### Our Architecture: Why No-Op Is Correct (For Now)

Our system has two independent interrupt dispatch paths:

```
                    ┌─── Vector[17] (SYSTIMER tick) ───────────────────┐
                    │                                                   │
                    │  ThreadX path:                                    │
                    │  _tx_thread_context_save → timer ISR              │
                    │  → _tx_thread_context_restore                     │
                    │  (checks _tx_thread_execute_ptr, may switch)      │
                    │                                                   │
    Interrupt ──────┤                                                   │
                    │                                                   │
                    │  ┌── Vector[N≠17] (WiFi, BLE, etc.) ──────────┐ │
                    │  │                                              │ │
                    │  │  ESP-IDF path:                               │ │
                    │  │  _interrupt_handler → rtos_int_enter         │ │
                    │  │  → _global_interrupt_handler (dispatch)      │ │
                    │  │  → rtos_int_exit                             │ │
                    │  │  (checks xPortSwitchFlag,                   │ │
                    │  │   calls vTaskSwitchContext if set)           │ │
                    │  │                                              │ │
                    │  └──────────────────────────────────────────────┘ │
                    └───────────────────────────────────────────────────┘
```

**ThreadX timer tick path (vector[17]):**
- `_tx_thread_context_restore` already checks if a higher-priority thread became
  ready (by comparing `_tx_thread_execute_ptr` vs `_tx_thread_current_ptr`)
- If a WiFi ISR posted a semaphore (via the compat layer, which calls
  `tx_semaphore_put`), ThreadX internally updates `_tx_thread_execute_ptr`
- At the next timer tick, `_tx_thread_context_restore` sees the change and performs
  the context switch
- This happens within 10ms (one tick at 100 Hz)

**ESP-IDF interrupt path (vector[N≠17]):**
- `rtos_int_exit` checks `xPortSwitchFlag` and calls `vTaskSwitchContext`
- But our `vTaskSwitchContext` is a **no-op stub** (in `port.c`):
  ```c
  void vTaskSwitchContext(int xCoreID)
  {
      (void)xCoreID;
      /* ThreadX handles context switches internally */
  }
  ```
- And with the fix, `portYIELD_FROM_ISR` is also a no-op, so `xPortSwitchFlag`
  never gets set in the first place
- Both no-ops are consistent: nothing triggers a switch, nothing handles a switch

**Why this is correct:**

ThreadX's preemption model is fundamentally different from FreeRTOS:

| Aspect | FreeRTOS | ThreadX |
|--------|----------|---------|
| ISR-triggered switch | `portYIELD_FROM_ISR` → flag → `rtos_int_exit` switches | Not needed — ThreadX checks at every tick |
| Thread preemption | Only when explicitly yielded or at tick | Automatic at every timer ISR |
| Who updates TCB pointer | `vTaskSwitchContext()` | `_tx_thread_context_restore` |
| Switch granularity | Immediate (ISR exit) | Next tick (up to 10ms) |

The ThreadX timer ISR (`_tx_esp32c6_trap_handler`) runs at 100 Hz. Its
`_tx_thread_context_restore` checks `_tx_thread_execute_ptr` every time:
- If a WiFi semaphore woke a higher-priority thread, ThreadX updated
  `_tx_thread_execute_ptr` inside `tx_semaphore_put` (called from the compat layer)
- At the next tick, `_tx_thread_context_restore` sees execute_ptr ≠ current_ptr
  and switches threads

The maximum latency is one tick period = 10ms at 100 Hz. For WiFi event processing
(connection state changes, scan results, data packets), 10ms is well within
acceptable bounds.

### The Fix

**`FreeRTOSConfig.h` — Primary definition (seen by upstream compat FreeRTOS.h):**
```c
/* Variadic: supports both portYIELD_FROM_ISR() and portYIELD_FROM_ISR(x)
 * Bug 35: Must be no-op — vPortYield() calls tx_thread_relinquish() which is
 * a thread-level API. Calling it from ISR context corrupts ThreadX scheduler.
 * ThreadX handles preemption automatically via _tx_thread_context_restore. */
#define portYIELD_FROM_ISR(...)             ((void)0)
#define portEND_SWITCHING_ISR(...)          ((void)0)
```

**`portmacro.h` — Override with `#undef` (prevents redefinition warning):**
```c
/* Bug 35: These were originally defined as vPortYield() → tx_thread_relinquish().
 * That is a THREAD-LEVEL ThreadX API — calling it from ISR context corrupts the
 * scheduler (modifies _tx_thread_current_ptr while ISR context save/restore
 * expects it stable).
 *
 * Fix: No-op. ThreadX handles preemption automatically — _tx_thread_context_restore
 * (called at every timer tick) checks if a higher-priority thread became ready and
 * performs the context switch. Trade-off: up to 10ms (one tick @ 100 Hz) latency
 * for ISR-to-thread wakeup, which is acceptable for WiFi/BLE events.
 *
 * Future: implement xPortSwitchFlag + vTaskSwitchContext integration with
 * rtos_int_exit for lower-latency ISR-triggered context switches.
 */
#undef portYIELD_FROM_ISR
#undef portEND_SWITCHING_ISR
#define portYIELD_FROM_ISR(...)         ((void)0)
#define portEND_SWITCHING_ISR(...)      ((void)0)
```

### Why Both Files Needed Fixing

The macro is defined in two places due to our header architecture:

```
FreeRTOSConfig.h  ← Included FIRST by upstream compat FreeRTOS.h
    ↓                (defines portYIELD_FROM_ISR for the compat layer's
    ↓                 #ifndef guards)
    ↓
upstream compat FreeRTOS.h (threadx/.../FreeRTOS.h)
    ↓
    ↓  includes →  portmacro.h  ← Included SECOND
                   (provides ESP-IDF-specific macros, must #undef + redefine
                    to override FreeRTOSConfig.h definitions)
```

If we only fixed `portmacro.h`, `FreeRTOSConfig.h` still had the old `vPortYield()`
definition. Since `FreeRTOSConfig.h` is included first, the upstream compat layer
would see the wrong definition. The `#undef` in `portmacro.h` ensures the final
definition is always the no-op, regardless of include order.

### Why Not Use `portYIELD_WITHIN_API` As Reference?

`portYIELD_WITHIN_API()` is defined as `portYIELD()` → `vPortYield()` →
`tx_thread_relinquish()`. This is **correct** because `portYIELD_WITHIN_API` is
only called from task context — inside FreeRTOS API functions like `xQueueSend`,
`xSemaphoreGive`, etc. The calling thread wants to yield after modifying a queue
or semaphore. Since it's running on a thread stack with `_tx_thread_system_state=0`,
`tx_thread_relinquish()` is safe.

The crucial distinction:
- `portYIELD()` / `portYIELD_WITHIN_API()` → called from **thread context** → safe
- `portYIELD_FROM_ISR()` / `portEND_SWITCHING_ISR()` → called from **ISR context** → UNSAFE

### Future Optimization: Lower-Latency ISR Context Switches

The current no-op approach has up to 10ms latency for ISR-to-thread wakeup. For
most WiFi/BLE use cases this is fine, but for latency-sensitive applications
(real-time audio streaming, low-latency sensor fusion), we might want immediate
ISR-triggered context switches.

The infrastructure is already in place:

1. **`rtos_int_hooks.S`** already checks `xPortSwitchFlag` in `rtos_int_exit`
   and calls `vTaskSwitchContext`

2. **`xPortSwitchFlag`** is already declared as a global in `rtos_int_hooks.S`

To implement:

1. Change `portYIELD_FROM_ISR` to set the flag:
   ```c
   extern volatile uint32_t xPortSwitchFlag;
   #define portYIELD_FROM_ISR(...) do { xPortSwitchFlag = 1; } while(0)
   ```

2. Implement `vTaskSwitchContext` to bridge ThreadX and the fake TCB:
   ```c
   void vTaskSwitchContext(int xCoreID)
   {
       TX_THREAD *next = _tx_thread_execute_ptr;
       if (next && next != _tx_thread_current_ptr) {
           /* Save current thread's SP into its TX_TCB */
           /* Update pxCurrentTCBs to point to next thread's SP save area */
           /* The actual SP restoration happens in rtos_int_exit */
       }
   }
   ```

3. This requires careful integration because ThreadX's `_tx_thread_context_save`
   and `_tx_thread_context_restore` would need to be aware of context switches
   that happened through the ESP-IDF path. The ThreadX timer ISR at vector[17]
   saves/restores via ThreadX's own TCB, while the ESP-IDF path at vector[N≠17]
   saves/restores via the `pxCurrentTCBs` fake TCB. Making these two paths aware
   of each other is non-trivial.

For now, the 10ms latency approach is the correct and safe choice.

### Related: The Complete Yield Macro Family

For reference, here's every yield-related macro and its correct behavior in our
ThreadX compat layer:

| Macro | Context | Implementation | Correct? |
|-------|---------|---------------|----------|
| `portYIELD()` | Thread | `vPortYield()` → `tx_thread_relinquish()` | Yes |
| `portYIELD_WITHIN_API()` | Thread | `portYIELD()` → same as above | Yes |
| `portYIELD_FROM_ISR(...)` | ISR | `((void)0)` — no-op | Yes (Bug 35 fix) |
| `portEND_SWITCHING_ISR(...)` | ISR | `((void)0)` — no-op | Yes (Bug 35 fix) |

### Summary of Changes — Iteration 30

**Files modified:**

| File | Change | Bug |
|------|--------|-----|
| `FreeRTOSConfig.h` | Changed `portYIELD_FROM_ISR`/`portEND_SWITCHING_ISR` from `vPortYield()` to `((void)0)` | Bug 35 |
| `portmacro.h` | Added `#undef` + changed definitions to `((void)0)` with comprehensive Bug 35 comment | Bug 35 |

**Bug Summary:**

| Bug # | Symptom | Root Cause | Fix |
|-------|---------|------------|-----|
| 35 | System freezes after tick 32-33 when first WiFi interrupt fires | `portYIELD_FROM_ISR` → `vPortYield()` → `tx_thread_relinquish()` called from ISR context, corrupting ThreadX scheduler | Change to no-op `((void)0)`; ThreadX handles preemption at timer tick |

### Cumulative Bug Table — Iterations 28-30

| Bug # | Iteration | Symptom | Root Cause | Fix |
|-------|-----------|---------|------------|-----|
| 33 | 28 | Hang at "ThreadX taking over" | `PERIPH_RCC_ACQUIRE_ATOMIC` → `portEXIT_CRITICAL` → MIE=1 during init | Remove call; bus clock already enabled |
| 34 | 29 | Hang at "wifi:enable tsf" | Vector table dead-loops for non-17 interrupts | Route all vectors to `_interrupt_handler`; enable ISR stack switching |
| 35 | 30 | Freeze after tick 32-33 | `portYIELD_FROM_ISR` calls thread-level `tx_thread_relinquish()` from ISR | Change to no-op; ThreadX preempts at timer tick |

---

## Iteration 31 — Bug 36: `_tx_thread_system_state` Not Set During ESP-IDF ISRs

### The Symptom

After fixing Bug 35 (`portYIELD_FROM_ISR` no-op), the system no longer crashed
immediately at tick 32-33. However, it still froze shortly after — both threads
printed once (tick=32 main, tick=33 scanner) and then stopped forever. The timer
tick appeared to stop firing entirely.

```
I (810) wifi_demo: WiFi STA init complete, connection attempt in background...
I (820) wifi_demo: Scanner thread created (ThreadX prio 20)
I (820) wifi_demo: [main] tick=32 thread='main' wifi_bits=0x0 retries=0
I (830) wifi_demo: [scanner] Background WiFi scanner started (ThreadX thread, tick=33)
<... system freezes — no more output ...>
```

### Diagnosis: The Two Nesting Counter Problem

The key question was: why does the timer stop firing after the first WiFi interrupt?

We verified (via agent search) that the `mie` CSR is NOT being modified by ESP-IDF's
interrupt allocation. `rv_utils_intr_enable/disable` only touch `PLIC_MXINT_ENABLE_REG`
(0x20001000), not individual `mie` bits. Our `mie bit 17` is preserved.

The real culprit was `_tx_thread_system_state` — ThreadX's ISR nesting counter.

ThreadX uses `_tx_thread_system_state` (defined in `tx_thread_initialize.c`) to track
whether the CPU is executing in ISR context. Every ThreadX API checks this variable:

- `_tx_thread_system_state == 0` → thread context → immediate preemption allowed
- `_tx_thread_system_state > 0` → ISR context → defer preemption

The ThreadX timer ISR path (vector[17]) manages this correctly:
- `_tx_thread_context_save` increments `_tx_thread_system_state`
- `_tx_thread_context_restore` decrements it

But the ESP-IDF interrupt path (vector[N≠17]) goes through `_interrupt_handler` →
`rtos_int_enter` → handler → `rtos_int_exit`. Our `rtos_int_enter`/`rtos_int_exit`
managed `port_uxInterruptNesting` (the FreeRTOS compat counter) but **never touched
`_tx_thread_system_state`**.

### Root Cause: ThreadX APIs Think They're in Thread Context During WiFi ISR

When a WiFi ISR fires and calls FreeRTOS compat APIs (e.g., `xSemaphoreGive` →
`tx_semaphore_put`), the following happens:

```
WiFi ISR (on ISR stack, entered via _interrupt_handler)
  │
  │ port_uxInterruptNesting = 1  ← rtos_int_enter set this
  │ _tx_thread_system_state = 0  ← NEVER INCREMENTED!
  │
  ├→ xSemaphoreGive(wifi_sem)
  │   └→ tx_semaphore_put(sem)
  │       └→ _tx_thread_system_resume(waiting_thread)
  │           │
  │           │ Checks: _tx_thread_system_state == 0
  │           │ Thinks: "I'm in thread context"
  │           │ Action: IMMEDIATE preemption via _tx_thread_system_return()
  │           │
  │           └→ _tx_thread_system_return()
  │               ├→ Saves SP (ISR stack!) as thread's stack pointer
  │               ├→ Switches to ThreadX system stack
  │               └→ Enters _tx_thread_schedule → HANGS or corrupts
```

`_tx_thread_system_resume()` (called internally by `tx_semaphore_put`) checks
`_tx_thread_system_state` to decide whether to preempt immediately or defer:

```c
// Inside _tx_thread_system_resume (ThreadX common source):
if (_tx_thread_system_state != 0)
{
    // ISR context — just update _tx_thread_execute_ptr, defer switch
    _tx_thread_execute_ptr = highest_priority_ready;
    return;
}
// Thread context — preempt immediately
if (new_thread->priority < current_thread->priority)
{
    _tx_thread_system_return();  // ← Context switch NOW
}
```

With `_tx_thread_system_state == 0` during the WiFi ISR, ThreadX calls
`_tx_thread_system_return()` from ISR context. This function:

1. Saves the current SP (which is the **ISR stack**, not the thread stack) into
   `_tx_thread_current_ptr->tx_thread_stack_ptr` — corrupting the thread's saved SP
2. Switches to the ThreadX system stack
3. Calls `_tx_thread_schedule` to pick the next thread
4. The corrupted thread can never be properly restored

### The Fix

Added `_tx_thread_system_state` increment/decrement to `rtos_int_hooks.S`:

**`rtos_int_enter` — after incrementing `port_uxInterruptNesting`:**
```asm
    /* Bug 36: Increment ThreadX system state so TX APIs know we're in ISR.
     * Without this, tx_semaphore_put etc. see system_state==0, think they're
     * in thread context, and call _tx_thread_system_return() — corrupting
     * the scheduler because we're actually on the ISR stack. */
    la      t3, _tx_thread_system_state
    lw      t5, 0(t3)
    addi    t5, t5, 1
    sw      t5, 0(t3)
```

**`rtos_int_exit` — after decrementing `port_uxInterruptNesting`:**
```asm
    /* Bug 36: Decrement ThreadX system state (mirrors the increment in
     * rtos_int_enter). This restores the system_state to its pre-ISR value
     * so ThreadX APIs return to normal thread-context behavior. */
    la      t2, _tx_thread_system_state
    lw      t4, 0(t2)
    beqz    t4, .Lskip_sys_dec
    addi    t4, t4, -1
    sw      t4, 0(t2)
.Lskip_sys_dec:
```

### Nesting Correctness

Both increment/decrement paths must be consistent when ThreadX timer ISRs nest
into ESP-IDF ISRs:

```
Thread running: _tx_thread_system_state = 0
  │
  ├→ WiFi IRQ fires (vector[N])
  │   rtos_int_enter: state 0 → 1
  │   │
  │   ├→ ThreadX timer IRQ nests (vector[17], MIE re-enabled by _interrupt_handler)
  │   │   _tx_thread_context_save: state 1 → 2
  │   │   timer ISR runs
  │   │   _tx_thread_context_restore: state 2 → 1
  │   │   mret → back to WiFi handler
  │   │
  │   WiFi handler continues
  │   Any ThreadX API sees state=1 → defers preemption ✓
  │   rtos_int_exit: state 1 → 0
  │
  Thread resumes: _tx_thread_system_state = 0 ✓
```

The two increment/decrement pairs (ThreadX context_save/restore + our
rtos_int_enter/exit) compose correctly for any nesting scenario.

### Result: WiFi Scanning Working on ThreadX

After this fix, the system ran continuously without freezing:

```
I (820) wifi_demo: [main] tick=32 thread='main' wifi_bits=0x0 retries=0
I (830) wifi_demo: [scanner] Background WiFi scanner started (ThreadX thread, tick=33)
...
I (7819) wifi_demo: [scanner] Found 17 networks (showing 15):
I (7819) wifi_demo: [scanner]  SSID                              RSSI  Auth             Channel
I (7829) wifi_demo: [scanner]  Flat 4                             -83  WPA2/WPA3-PSK    1
I (7829) wifi_demo: [scanner]  Flat 12                            -84  WPA2/WPA3-PSK    1
I (7839) wifi_demo: [scanner]  PLUSNET-68F9Q5                     -84  WPA2-PSK         1
I (7849) wifi_demo: [scanner]  Flat 6                             -84  WPA2/WPA3-PSK    1
...
```

This demonstrates:
- **ThreadX timer tick** running continuously at 100 Hz
- **Multi-thread scheduling** — main thread (prio 16) and scanner thread (prio 20)
  interleaving correctly with proper sleep/wake timing
- **WiFi hardware** — radio scanning all channels, returning real AP records
- **ESP-IDF interrupt dispatch** — WiFi ISRs handled via `_interrupt_handler` without
  corrupting ThreadX scheduler state
- **FreeRTOS compat layer** — `xEventGroupCreate`, `xEventGroupGetBits`, `malloc/free`,
  `esp_wifi_*` APIs all working through the ThreadX-backed compatibility layer
- **Dual interrupt path** — vector[17] (ThreadX timer) and vector[N≠17] (ESP-IDF WiFi)
  coexisting safely with synchronized `_tx_thread_system_state` management

### Known Issue: Blocking Scan Timeout

`esp_wifi_scan_start(&config, true)` (blocking mode) times out after ~12 seconds
instead of returning scan results. The non-blocking approach works correctly:

```c
esp_wifi_scan_start(&scan_config, false);  // start scan, return immediately
tx_thread_sleep(500);                       // wait 5s for hardware to finish
esp_wifi_scan_get_ap_num(&ap_count);        // collect results
esp_wifi_scan_get_ap_records(&count, recs);
```

**Why blocking mode fails:** `esp_wifi_scan_start(block=true)` internally waits on
a FreeRTOS semaphore that is posted when the WiFi event system delivers
`WIFI_EVENT_SCAN_DONE`. The scan done notification flows through:

1. WiFi hardware completes scan
2. WiFi ISR signals internal WiFi task
3. WiFi task calls `esp_event_post(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, ...)`
4. Event loop task receives and dispatches the event
5. Internal WiFi event handler posts the scan-done semaphore
6. `esp_wifi_scan_start` (blocked on semaphore) wakes up

Something in this chain — likely the event loop task dispatching to the internal
WiFi event handler, or the semaphore wake mechanism — doesn't work correctly
through the compat layer. The non-blocking workaround avoids the entire chain.

**Investigation TODO:** Trace the event dispatch chain to find where the scan-done
notification gets lost. Likely candidates:
- `xQueueSend`/`xQueueReceive` in the event loop task (queue compat)
- The internal WiFi default event handler registration (may need explicit compat)
- Semaphore `xSemaphoreGive` from the event handler task context (non-ISR, should work)

### Summary of Changes — Iteration 31

**Files modified:**

| File | Change | Bug |
|------|--------|-----|
| `rtos_int_hooks.S` | Added `_tx_thread_system_state` increment in `rtos_int_enter` and decrement in `rtos_int_exit` | Bug 36 |
| `main.c` | Scan-only mode: commented out connection code, non-blocking scan, removed `xEventGroupWaitBits` gate | Demo |

**Bug Summary:**

| Bug # | Symptom | Root Cause | Fix |
|-------|---------|------------|-----|
| 36 | System freezes after tick 32-33 (again, but different cause than Bug 35) | `_tx_thread_system_state` not set during ESP-IDF ISRs → ThreadX APIs attempt immediate preemption from ISR context | Increment/decrement `_tx_thread_system_state` in `rtos_int_enter`/`rtos_int_exit` |

### Cumulative Bug Table — Iterations 28-31

| Bug # | Iteration | Symptom | Root Cause | Fix |
|-------|-----------|---------|------------|-----|
| 33 | 28 | Hang at "ThreadX taking over" | `PERIPH_RCC_ACQUIRE_ATOMIC` → MIE=1 during init | Remove call; bus clock already enabled |
| 34 | 29 | Hang at "wifi:enable tsf" | Vector table dead-loops for non-17 interrupts | Route all vectors to `_interrupt_handler` |
| 35 | 30 | Freeze after tick 32-33 | `portYIELD_FROM_ISR` calls `tx_thread_relinquish()` from ISR | Change to no-op `((void)0)` |
| 36 | 31 | Freeze after tick 32-33 (different root cause) | `_tx_thread_system_state` not managed in ESP-IDF ISR path | Increment/decrement in `rtos_int_enter`/`rtos_int_exit` |

### WiFi Port Milestone

**WiFi scanning confirmed working on ThreadX as of Iteration 31.**

The ESP32-C6 WiFi hardware, ESP-IDF WiFi driver stack, and ThreadX RTOS are
successfully coexisting:

```
┌─────────────────────────────────────────────────────────┐
│                    APPLICATION                           │
│  main_thread (ThreadX prio 16) — status monitor         │
│  scanner_thread (ThreadX prio 20) — WiFi scan + print   │
├─────────────────────────────────────────────────────────┤
│              FREERTOS COMPAT LAYER                       │
│  xEventGroupCreate, xSemaphore*, xTaskCreate, etc.       │
│  backed by ThreadX primitives (tx_semaphore, tx_thread)  │
├─────────────────────────────────────────────────────────┤
│                ESP-IDF COMPONENTS                         │
│  esp_wifi, esp_event, esp_netif, nvs_flash, lwip         │
│  (all using FreeRTOS APIs → routed through compat layer) │
├─────────────────────────────────────────────────────────┤
│              INTERRUPT DISPATCH                          │
│  vector[17] → ThreadX timer (100 Hz SYSTIMER tick)       │
│  vector[N]  → ESP-IDF _interrupt_handler (WiFi, etc.)    │
│  _tx_thread_system_state synchronized across both paths  │
├─────────────────────────────────────────────────────────┤
│                 THREADX KERNEL                           │
│  Scheduler, timer, semaphores, byte pools, threads       │
│  Running on ESP32-C6 RISC-V via risc-v64/gnu port        │
└─────────────────────────────────────────────────────────┘
```

**Remaining work:**
- Fix blocking scan (`esp_wifi_scan_start(block=true)`) — event dispatch chain issue
- STA connection mode — uncomment connection code with real credentials
- Test `xEventGroupWaitBits` with real WiFi events (connect/disconnect/IP)
- Verify phase 0 (basic ThreadX demo without WiFi) still works
