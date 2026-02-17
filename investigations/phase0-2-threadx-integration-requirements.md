# Phase 0.2: ThreadX Integration Requirements Investigation

**Status:** Complete
**Date:** 2026-02-17
**Scope:** ThreadX RISC-V port feasibility, API mapping, memory model, initialization flow

---

## 1. ThreadX Overview and Licensing

### 1.1 Current Status

ThreadX was originally developed by Express Logic, acquired by Microsoft in 2019 (as Azure RTOS), then donated to the **Eclipse Foundation** in 2023 under the **MIT license**.

- **Repository:** [https://github.com/eclipse-threadx/threadx](https://github.com/eclipse-threadx/threadx)
- **License:** MIT (fully open source, royalty-free)
- **Documentation:** [https://learn.microsoft.com/en-us/azure/rtos/threadx/](https://learn.microsoft.com/en-us/azure/rtos/threadx/)
- **Eclipse Project:** [https://threadx.io/](https://threadx.io/)

### 1.2 ThreadX Middleware Ecosystem

| Component | Purpose | Relevant for ESP32-C6? |
|-----------|---------|----------------------|
| ThreadX | RTOS Kernel | Yes (core) |
| NetX Duo | TCP/IP Stack | Maybe (alternative to lwIP) |
| FileX | File System | Maybe (alternative to SPIFFS/LittleFS) |
| USBX | USB Stack | Yes (USB Serial/JTAG on C6) |
| LevelX | Flash Wear Leveling | Maybe |
| GUIX | GUI Framework | No (C6 has no display) |
| TraceX | Event Tracing Tool | Yes (debugging) |

**Reference:** [https://github.com/eclipse-threadx](https://github.com/eclipse-threadx)

---

## 2. ThreadX RISC-V Port Status

### 2.1 Existing RISC-V Ports

ThreadX has official RISC-V ports in the repository:

```
threadx/ports/
├── risc-v32/
│   ├── gnu/
│   │   ├── tx_initialize_low_level.S
│   │   ├── tx_thread_context_save.S
│   │   ├── tx_thread_context_restore.S
│   │   ├── tx_thread_interrupt_control.S
│   │   ├── tx_thread_schedule.S
│   │   ├── tx_thread_stack_build.S
│   │   ├── tx_thread_system_return.S
│   │   ├── tx_timer_interrupt.S
│   │   └── tx_port.h
│   └── iar/
│       └── [IAR compiler versions]
└── risc-v64/
    └── [64-bit RISC-V ports]
```

**Key finding:** The `risc-v32/gnu/` port targets **RV32I** with GCC toolchain, which is compatible with ESP32-C6's **RV32IMAC** ISA.

**Reference:** [https://github.com/eclipse-threadx/threadx/tree/master/ports/risc-v32](https://github.com/eclipse-threadx/threadx/tree/master/ports/risc-v32)

### 2.2 Port Architecture Details

The RISC-V 32-bit port uses:
- **Machine mode** (M-mode) execution
- Standard RISC-V CSRs for context management
- `mtvec` for interrupt vector setup
- `mstatus` for interrupt enable/disable
- `mscratch` for thread context pointer during interrupts
- `mcause` for interrupt/exception identification

### 2.3 What Needs Adaptation for ESP32-C6

| Aspect | ThreadX Port (Generic RV32) | ESP32-C6 Specific | Adaptation Needed |
|--------|---------------------------|-------------------|-------------------|
| ISA | RV32I | RV32IMAC | Minimal (superset) |
| Interrupt Controller | Generic PLIC | ESP32-C6 INTC/PLIC | Yes - register addresses |
| Timer | Standard mtime/mtimecmp | SYSTIMER peripheral | Yes - timer source |
| Memory Map | Generic | ESP32-C6 specific | Yes - linker script |
| Cache | None assumed | ICache + DCache | May need cache ops |
| Boot | Generic startup | ESP-IDF 2nd stage bootloader | Yes - startup code |
| Toolchain | Generic riscv32-gcc | riscv32-esp-elf-gcc | Verify compatibility |

---

## 3. ThreadX Initialization Flow

### 3.1 Standard Boot Sequence

```
Hardware Reset
    |
    v
_tx_initialize_low_level()        <-- Port-specific (assembly)
    |  - Set up interrupt vectors (mtvec)
    |  - Configure stack pointers
    |  - Set up system timer
    |  - Initialize hardware
    |
    v
_tx_initialize_kernel_setup()      <-- ThreadX internal
    |  - Initialize internal data structures
    |  - Set up thread scheduling
    |
    v
tx_application_define()            <-- User callback
    |  - Create threads, queues, semaphores, etc.
    |  - Set up application objects
    |
    v
tx_kernel_enter()                  <-- Start scheduler
    |  - Enable interrupts
    |  - Start first thread
    |  - Never returns
    v
[ThreadX Scheduler Running]
```

### 3.2 ESP32-C6 Adaptation

For ESP32-C6, we need to integrate with ESP-IDF's boot sequence:

```
ROM Bootloader (in chip ROM)
    |
    v
2nd Stage Bootloader (ESP-IDF)
    |  - Flash decryption
    |  - Secure boot verification
    |  - OTA partition selection
    |
    v
app_main() (ESP-IDF entry point)
    |
    v
ThreadX initialization
    |  - _tx_initialize_low_level() adapted for C6
    |  - Configure SYSTIMER for ThreadX tick
    |  - Set up interrupt routing via ESP-IDF HAL
    |
    v
tx_application_define()
    |
    v
tx_kernel_enter()
```

**Key insight:** We use ESP-IDF's bootloader and startup code, then hand off to ThreadX from `app_main()`. This preserves secure boot, flash encryption, and OTA compatibility.

---

## 4. FreeRTOS to ThreadX API Mapping

### 4.1 Thread/Task Management

| FreeRTOS API | ThreadX API | Notes |
|---|---|---|
| `xTaskCreate()` | `tx_thread_create()` | Stack allocation differs: FreeRTOS allocates internally, ThreadX takes user-provided buffer |
| `xTaskCreateStatic()` | `tx_thread_create()` | ThreadX always uses static allocation model |
| `xTaskCreatePinnedToCore()` | `tx_thread_create()` | N/A on single-core C6, ignore core param |
| `vTaskDelete()` | `tx_thread_terminate()` + `tx_thread_delete()` | ThreadX requires two-step: terminate then delete |
| `vTaskDelay()` | `tx_thread_sleep()` | Direct mapping, tick units |
| `vTaskDelayUntil()` | Custom implementation needed | Use `tx_thread_sleep()` with time calculation |
| `vTaskSuspend()` | `tx_thread_suspend()` | Direct mapping |
| `vTaskResume()` | `tx_thread_resume()` | Direct mapping |
| `uxTaskPriorityGet()` | `tx_thread_info_get()` | Priority info available |
| `vTaskPrioritySet()` | `tx_thread_priority_change()` | Direct mapping |
| `taskYIELD()` | `tx_thread_relinquish()` | Direct mapping |
| `xTaskGetCurrentTaskHandle()` | `tx_thread_identify()` | Direct mapping |
| `pcTaskGetName()` | `thread_ptr->tx_thread_name` | Access name field directly |
| `vTaskStartScheduler()` | `tx_kernel_enter()` | Direct mapping |

### 4.2 Semaphores

| FreeRTOS API | ThreadX API | Notes |
|---|---|---|
| `xSemaphoreCreateBinary()` | `tx_semaphore_create(sem, name, 0)` | Initial count = 0 |
| `xSemaphoreCreateCounting()` | `tx_semaphore_create(sem, name, init)` | Direct mapping |
| `xSemaphoreTake()` | `tx_semaphore_get(sem, timeout)` | Timeout mapping: `portMAX_DELAY` -> `TX_WAIT_FOREVER` |
| `xSemaphoreGive()` | `tx_semaphore_put(sem)` | Direct mapping |
| `xSemaphoreGiveFromISR()` | `tx_semaphore_put(sem)` | ThreadX is ISR-safe by default |
| `vSemaphoreDelete()` | `tx_semaphore_delete(sem)` | Direct mapping |

### 4.3 Mutexes

| FreeRTOS API | ThreadX API | Notes |
|---|---|---|
| `xSemaphoreCreateMutex()` | `tx_mutex_create(mutex, name, TX_INHERIT)` | Priority inheritance available |
| `xSemaphoreCreateRecursiveMutex()` | `tx_mutex_create(mutex, name, TX_INHERIT)` | ThreadX mutexes are recursive by default |
| `xSemaphoreTake()` (mutex) | `tx_mutex_get(mutex, timeout)` | Same timeout mapping |
| `xSemaphoreGive()` (mutex) | `tx_mutex_put(mutex)` | Direct mapping |

### 4.4 Queues

| FreeRTOS API | ThreadX API | Notes |
|---|---|---|
| `xQueueCreate()` | `tx_queue_create()` | **Significant difference:** ThreadX queue messages are in 32-bit words, FreeRTOS uses bytes. Need wrapper to handle arbitrary message sizes |
| `xQueueSend()` | `tx_queue_send(q, src, timeout)` | Message size handling needed |
| `xQueueSendToFront()` | `tx_queue_front_send()` | Direct mapping |
| `xQueueReceive()` | `tx_queue_receive(q, dst, timeout)` | Message size handling needed |
| `xQueueSendFromISR()` | `tx_queue_send()` | ThreadX is ISR-safe |
| `uxQueueMessagesWaiting()` | Query via `tx_queue_info_get()` | Indirect |
| `vQueueDelete()` | `tx_queue_delete()` | Direct mapping |

**Queue implementation note:** ThreadX queues work in 32-bit word multiples (1, 2, 4, 8, or 16 words per message). For FreeRTOS compatibility with arbitrary byte-sized messages, we need a wrapper that either:
- Rounds up to next word boundary (wastes some memory)
- Uses byte pools for large/variable messages

### 4.5 Timers

| FreeRTOS API | ThreadX API | Notes |
|---|---|---|
| `xTimerCreate()` | `tx_timer_create()` | Callback model is similar |
| `xTimerStart()` | `tx_timer_activate()` | Direct mapping |
| `xTimerStop()` | `tx_timer_deactivate()` | Direct mapping |
| `xTimerReset()` | `tx_timer_change()` + `tx_timer_activate()` | Two-step |
| `xTimerChangePeriod()` | `tx_timer_change()` | Direct mapping |
| `xTimerDelete()` | `tx_timer_delete()` | Direct mapping |
| `pvTimerGetTimerID()` | Custom: store in timer structure | Need wrapper |

### 4.6 Event Groups / Event Flags

| FreeRTOS API | ThreadX API | Notes |
|---|---|---|
| `xEventGroupCreate()` | `tx_event_flags_create()` | Direct mapping |
| `xEventGroupSetBits()` | `tx_event_flags_set(grp, flags, TX_OR)` | Direct mapping |
| `xEventGroupClearBits()` | `tx_event_flags_set(grp, ~flags, TX_AND)` | Inverted logic |
| `xEventGroupWaitBits()` | `tx_event_flags_get()` | Map wait-for-all/any to `TX_AND`/`TX_OR` |
| `xEventGroupSetBitsFromISR()` | `tx_event_flags_set()` | ThreadX is ISR-safe |
| `vEventGroupDelete()` | `tx_event_flags_delete()` | Direct mapping |

### 4.7 Critical Sections / Interrupt Control

| FreeRTOS API | ThreadX API | Notes |
|---|---|---|
| `portENTER_CRITICAL()` | `TX_DISABLE` / `tx_interrupt_control(TX_INT_DISABLE)` | Disable interrupts |
| `portEXIT_CRITICAL()` | `TX_RESTORE` / `tx_interrupt_control(old_posture)` | Restore interrupts |
| `portENTER_CRITICAL_ISR()` | Same as above | On single-core, same behavior |
| `taskENTER_CRITICAL()` | Same as `portENTER_CRITICAL` | Alias |
| `vPortEnterCritical()` | Nesting counter + disable | Need to match FreeRTOS nesting behavior |
| `vPortExitCritical()` | Nesting counter + restore | Need to match FreeRTOS nesting behavior |

### 4.8 Memory Management

| FreeRTOS API | ThreadX API | Notes |
|---|---|---|
| `pvPortMalloc()` | `tx_byte_allocate()` | From byte pool |
| `vPortFree()` | `tx_byte_release()` | Direct mapping |
| `xPortGetFreeHeapSize()` | `tx_byte_pool_info_get()` | Available bytes |

### 4.9 Task Notifications (Complex Mapping)

FreeRTOS task notifications have no direct ThreadX equivalent. Implementation options:

| FreeRTOS API | ThreadX Implementation | Approach |
|---|---|---|
| `xTaskNotifyGive()` | Per-thread semaphore | Allocate semaphore per thread for notification |
| `ulTaskNotifyTake()` | `tx_semaphore_get()` | On per-thread semaphore |
| `xTaskNotify()` | Per-thread event flags | Use event flags for value-based notifications |
| `xTaskNotifyWait()` | `tx_event_flags_get()` | On per-thread event flags |

---

## 5. ThreadX Memory Model

### 5.1 Memory Pool Types

ThreadX provides two types of memory pools:

**Byte Pools** (`TX_BYTE_POOL`):
- Variable-size allocation (like malloc)
- Uses first-fit algorithm
- Can fragment over time
- Suitable for general-purpose allocation

**Block Pools** (`TX_BLOCK_POOL`):
- Fixed-size block allocation
- No fragmentation
- O(1) allocation and release
- Suitable for fixed-size objects (queue messages, control blocks)

### 5.2 Stack Management

ThreadX requires **user-provided stack buffers** for each thread:

```c
// ThreadX approach - user provides stack
static UCHAR thread_stack[2048];
tx_thread_create(&thread, "name", entry_fn, 0,
                 thread_stack, sizeof(thread_stack),
                 priority, preempt_threshold, time_slice, auto_start);
```

vs FreeRTOS which can allocate stacks internally:
```c
// FreeRTOS approach - allocates stack from heap
xTaskCreate(entry_fn, "name", 2048/4, NULL, priority, &handle);
```

**Compatibility layer impact:** The shim needs to `tx_byte_allocate()` a stack buffer when `xTaskCreate()` is called.

### 5.3 Object Allocation

ThreadX control blocks (TCB, semaphore, mutex, etc.) are **statically allocated** by the user:

```c
static TX_THREAD my_thread;          // Thread control block
static TX_SEMAPHORE my_sem;          // Semaphore control block
static TX_QUEUE my_queue;            // Queue control block
```

For FreeRTOS compatibility (which returns opaque handles from heap), the shim needs to allocate control blocks from a byte pool.

---

## 6. ThreadX Unique Features (Advantages over FreeRTOS)

### 6.1 Preemption Threshold

ThreadX offers **preemption-threshold scheduling** -- a thread can be preempted only by threads with priority higher than its preemption threshold, not just higher than its priority.

```c
// Thread with priority 10, preemption threshold 5
// Can be preempted only by threads with priority < 5
tx_thread_create(&thread, "name", entry, 0, stack, size,
                 10,    // priority
                 5,     // preemption_threshold (unique to ThreadX)
                 TX_NO_TIME_SLICE, TX_AUTO_START);
```

This reduces context switches and can improve real-time determinism.

### 6.2 Priority Inversion Protection

ThreadX mutexes support **priority inheritance** natively (optional per mutex):

```c
tx_mutex_create(&mutex, "name", TX_INHERIT);  // With priority inheritance
tx_mutex_create(&mutex, "name", TX_NO_INHERIT);  // Without
```

### 6.3 Event Chaining

ThreadX supports **notify callbacks** on most objects (semaphores, queues, event flags, timers). This allows event-driven programming without polling.

### 6.4 Deterministic Performance

ThreadX guarantees O(1) for most operations:
- Thread scheduling: O(1)
- Semaphore get/put: O(1)
- Mutex get/put: O(1)
- Queue send/receive: O(1)
- Context switch: Fixed time

---

## 7. RISC-V Specifics

### 7.1 Privilege Mode

ThreadX RISC-V port runs in **Machine mode (M-mode)**. This is appropriate for ESP32-C6 which is a bare-metal MCU without MMU.

### 7.2 CSR Usage

| CSR | Usage in ThreadX |
|-----|-----------------|
| `mstatus` | Interrupt enable/disable (MIE bit) |
| `mtvec` | Interrupt vector base address |
| `mepc` | Exception program counter (saved during interrupt) |
| `mcause` | Interrupt/exception cause identification |
| `mscratch` | Scratch register for interrupt handler (saves SP) |
| `mie` | Individual interrupt enable bits |
| `mip` | Pending interrupts |

### 7.3 Context Switch

ThreadX RISC-V context switch saves/restores:
- All 31 general-purpose registers (x1-x31)
- `mepc` (return address after interrupt)
- `mstatus` (interrupt state)
- Stack pointer management via `mscratch`

### 7.4 Timer Interrupt

The standard RISC-V timer (`mtime`/`mtimecmp`) drives the ThreadX system tick. For ESP32-C6, we need to adapt this to use the **SYSTIMER** peripheral instead.

**Reference:**
- [RISC-V Privileged Specification](https://riscv.org/specifications/privileged-isa/)
- [ThreadX RISC-V port source](https://github.com/eclipse-threadx/threadx/tree/master/ports/risc-v32/gnu)

---

## 8. Existing RISC-V MCU Examples

### 8.1 Known ThreadX RISC-V Deployments

| MCU | Vendor | ISA | Notes |
|-----|--------|-----|-------|
| GD32VF103 | GigaDevice | RV32IMAC | Most common ThreadX RISC-V target |
| FE310 | SiFive | RV32IMAC | Reference implementation |
| CH32V | WCH | RV32IMAC | Low-cost RISC-V MCU |
| BL602/BL616 | Bouffalo Lab | RV32IMAC | WiFi SoC (similar use case to ESP32-C6!) |

**BL602/BL616 is particularly relevant** -- it's a RISC-V WiFi SoC with ThreadX-like RTOS integration challenges similar to ours.

### 8.2 Reference Implementations to Study

1. **GigaDevice GD32VF103 BSP**: Well-documented ThreadX port for RV32IMAC
   - [https://github.com/eclipse-threadx/threadx/tree/master/ports/risc-v32/gnu](https://github.com/eclipse-threadx/threadx/tree/master/ports/risc-v32/gnu)

2. **SiFive Freedom-E SDK**: RISC-V SDK with RTOS examples
   - [https://github.com/sifive/freedom-e-sdk](https://github.com/sifive/freedom-e-sdk)

---

## 9. ThreadX vs FreeRTOS Architectural Differences

| Feature | FreeRTOS | ThreadX |
|---------|----------|---------|
| **Scheduling** | Priority-based preemptive | Priority-based preemptive + preemption threshold |
| **Priority numbering** | Lower number = lower priority | Lower number = HIGHER priority |
| **Max priorities** | configMAX_PRIORITIES (typically 25) | TX_MAX_PRIORITIES (default 32, up to 1024) |
| **Stack allocation** | Internal (from heap) or static | Always user-provided buffer |
| **Object creation** | Dynamic (heap) or static | Always static (user provides control block) |
| **Queue messages** | Arbitrary byte size | 32-bit word multiples (1-16 words) |
| **Timer callbacks** | From timer task context | From timer ISR context (configurable) |
| **Task deletion** | Single call | Two-step: terminate + delete |
| **ISR API** | Separate `*FromISR()` variants | Same API from task and ISR |
| **Memory pools** | heap_1 through heap_5 schemes | Byte pools + Block pools |
| **Code size** | ~6-10 KB | ~2-6 KB |
| **Context switch** | ~2-4 us typical | ~1-2 us typical |

### Priority Mapping (CRITICAL)

FreeRTOS: 0 = lowest, `configMAX_PRIORITIES-1` = highest
ThreadX: 0 = highest, `TX_MAX_PRIORITIES-1` = lowest

**Mapping formula:**
```c
threadx_priority = (TX_MAX_PRIORITIES - 1) - freertos_priority;
```

---

## 10. Feasibility Assessment

### 10.1 GO/NO-GO Evaluation

| Criteria | Status | Details |
|----------|--------|---------|
| RISC-V 32-bit port exists | **GO** | Official port at `ports/risc-v32/gnu/` |
| ISA compatibility (RV32IMAC) | **GO** | Port targets RV32I, IMAC is superset |
| Toolchain compatibility | **GO** | Both use GCC, ESP-IDF riscv32-esp-elf-gcc is compatible |
| API mapping feasible | **GO** | All FreeRTOS APIs have ThreadX equivalents or can be shimmed |
| Memory model compatible | **GO** | Byte pools can emulate FreeRTOS heap |
| Timer integration | **NEEDS WORK** | Must adapt from mtime to ESP32-C6 SYSTIMER |
| Interrupt controller | **NEEDS WORK** | Must adapt to ESP32-C6 INTC/PLIC specifics |
| MIT license | **GO** | No licensing concerns |

### 10.2 Overall: **GO** with targeted adaptations

The ThreadX RISC-V port is well-suited for ESP32-C6. Main work items:
1. Adapt timer source (SYSTIMER instead of mtime)
2. Adapt interrupt controller (ESP32-C6 INTC)
3. Create linker script for ESP32-C6 memory layout
4. Integrate with ESP-IDF bootloader startup sequence
5. Build FreeRTOS API compatibility layer (biggest effort)

---

## 11. References

### ThreadX / Eclipse ThreadX

1. [Eclipse ThreadX GitHub Organization](https://github.com/eclipse-threadx)
2. [ThreadX Source Repository](https://github.com/eclipse-threadx/threadx)
3. [ThreadX RISC-V 32-bit Port](https://github.com/eclipse-threadx/threadx/tree/master/ports/risc-v32)
4. [ThreadX User Guide (Microsoft)](https://learn.microsoft.com/en-us/azure/rtos/threadx/chapter1)
5. [ThreadX API Reference](https://learn.microsoft.com/en-us/azure/rtos/threadx/chapter4)
6. [ThreadX Porting Guide](https://learn.microsoft.com/en-us/azure/rtos/threadx/chapter5)
7. [ThreadX.io (Eclipse Foundation)](https://threadx.io/)

### RISC-V Architecture

8. [RISC-V Privileged Specification](https://riscv.org/specifications/privileged-isa/)
9. [RISC-V Unprivileged Specification](https://riscv.org/specifications/)
10. [SiFive Freedom-E SDK](https://github.com/sifive/freedom-e-sdk)

### Comparison / Migration

11. [FreeRTOS API Reference](https://www.freertos.org/a00106.html)
12. [FreeRTOS vs ThreadX Comparison](https://learn.microsoft.com/en-us/azure/rtos/threadx/overview-threadx)

### Related RISC-V RTOS Ports

13. [Zephyr RISC-V Architecture Support](https://docs.zephyrproject.org/latest/hardware/arch/risc-v.html)
14. [RT-Thread RISC-V Port](https://github.com/RT-Thread/rt-thread/tree/master/libcpu/risc-v)

---

*All URLs should be verified by the reader. ThreadX repository structure may change as the Eclipse Foundation evolves the project.*
