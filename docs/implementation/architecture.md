# Architecture and Boot Flow

## 1. ESP32-C6 Basics

The ESP32-C6 is a single-core RISC-V microcontroller:
- **ISA**: RV32IMAC (32-bit, integer, multiply, atomic, compressed)
- **Privilege**: Machine mode only (no supervisor/user mode used)
- **Flash**: Code executes from flash (XIP) or IRAM
- **No standard RISC-V peripherals**: No mtime, no mtimecmp, no standard PLIC

## 2. Startup Sequence

### Phase 1: ESP-IDF Boot (unchanged)

```
ROM bootloader
  └─> 2nd stage bootloader
        └─> app_main image loaded
              └─> start_cpu0 (assembler)
                    └─> start_cpu0_default (C)
                          ├─> Hardware init (UART, flash, heap, ...)
                          ├─> esp_newlib_init()
                          ├─> esp_log_init()
                          └─> esp_startup_start_app()   ← hook point
```

`esp_startup_start_app()` is declared **weak** in ESP-IDF. Normally it starts the
FreeRTOS scheduler. We override it with a **strong symbol** in `tx_port_startup.c`.

### Phase 2: ThreadX Takeover

```
esp_startup_start_app()       [tx_port_startup.c — strong symbol override]
  └─> tx_kernel_enter()       [ThreadX internals]
        ├─> _tx_initialize_kernel_setup()
        ├─> _tx_initialize_low_level()    [tx_initialize_low_level.S]
        │     ├─> Save system stack pointer
        │     ├─> Set __tx_free_memory_start
        │     ├─> Install mtvec trap handler
        │     ├─> Enable mie.MEIE (machine external interrupts)
        │     └─> _tx_port_setup_timer_interrupt()  [tx_timer_interrupt.c]
        │           ├─> Configure SYSTIMER COMP0 (16 MHz / 100 Hz = 160000 ticks)
        │           ├─> Route SYSTIMER_TARGET0 (source 57) → CPU int 7 via INTMTX
        │           └─> Enable CPU int 7 in PLIC MX registers
        ├─> tx_application_define()       [tx_port_startup.c]
        │     ├─> Create 32 KB system byte pool
        │     └─> Create main thread (priority 16) → calls app_main()
        └─> _tx_thread_schedule()         [tx_thread_schedule.S]
              └─> Picks highest-priority ready thread and starts it
                    └─> app_main() runs
                          └─> Creates "blink" thread (priority 5)
```

## 3. Component Relationships

```
┌─────────────────────────────────────────┐
│  main.c  (application)                  │
│  - Creates threads                       │
│  - Calls tx_thread_sleep()               │
└────────────────┬────────────────────────┘
                 │ uses
┌────────────────▼────────────────────────┐
│  ThreadX Kernel  (components/threadx/)  │
│                                         │
│  ┌──────────────────────────────────┐   │
│  │  threadx/common/src/*.c          │   │
│  │  (scheduler, timers, semaphores, │   │
│  │   mutexes, queues, byte pools)   │   │
│  └──────────────────────────────────┘   │
│                                         │
│  ┌──────────────────────────────────┐   │
│  │  risc-v64/gnu port (assembly)    │   │
│  │  - tx_thread_schedule.S          │   │
│  │  - tx_thread_context_save.S      │   │
│  │  - tx_thread_context_restore.S   │   │
│  │  - tx_thread_stack_build.S       │   │
│  │  - tx_thread_system_return.S     │   │
│  │  - tx_thread_interrupt_control.S │   │
│  └──────────────────────────────────┘   │
│                                         │
│  ┌──────────────────────────────────┐   │
│  │  ESP32-C6 port (our files)       │   │
│  │  - tx_initialize_low_level.S     │   │
│  │  - tx_timer_interrupt.c          │   │
│  │  - tx_port_startup.c             │   │
│  └──────────────────────────────────┘   │
└─────────────────────────────────────────┘
                 │ uses
┌────────────────▼────────────────────────┐
│  ESP-IDF HAL / SoC layer               │
│  - UART (via esp_log)                   │
│  - SYSTIMER peripheral                  │
│  - Interrupt Matrix                     │
│  - PLIC                                 │
└─────────────────────────────────────────┘
```

## 4. Thread Model

ThreadX priorities: 0 = highest, (TX_MAX_PRIORITIES-1) = lowest.

| Thread       | Priority | Stack   | Sleep     | Purpose         |
|-------------|----------|---------|-----------|-----------------|
| main        | 16       | 4096 B  | 200 ticks | Demo loop       |
| blink       | 5        | 2048 B  | 100 ticks | Demo loop       |
| tx_timer_thread | varies | internal | — | ThreadX timer expiry |

At 100 Hz tick rate: 100 ticks = 1 second, 200 ticks = 2 seconds.

## 5. Memory Layout

```
┌───────────────────────────────┐ ← Stack top (high address)
│  ESP-IDF startup stack        │   Used during boot phase
│  _tx_thread_system_stack_ptr  │   Saved here; ISRs run on this stack
├───────────────────────────────┤
│  ThreadX kernel data          │   .bss / .data
│  Thread control blocks        │
│  system_byte_pool_storage     │   32 KB static array in .bss
├───────────────────────────────┤
│  main_thread_stack            │   4096 bytes static in .bss
│  blink_thread_stack           │   2048 bytes static in .bss
├───────────────────────────────┤
│  Code (.flash.text / IRAM)    │   ThreadX hot paths in IRAM
└───────────────────────────────┘ ← 0x40800000 (IRAM start)
```

ThreadX critical code paths (scheduler, context switch, ISR entry/exit) are
placed in IRAM via `linker.lf` to avoid flash access latency during interrupts.

---

## 7. Project Layout and Examples Structure

The repository is structured so that `components/` are shared across multiple
independent example projects under `examples/`. Each example is a standalone
ESP-IDF project — buildable independently with its own menuconfig, sdkconfig,
and build directory.

```
threadx-esp32c6-project/
│
├── components/                      ← shared components (used by all examples)
│   ├── threadx/                     ← ThreadX kernel + ESP32-C6 port
│   │   ├── Kconfig                  ← RTOS selection + ThreadX config
│   │   ├── CMakeLists.txt           ← early-return guard when FREERTOS selected
│   │   ├── include/tx_user.h        ← ThreadX config overrides (uses CONFIG_* macros)
│   │   ├── port/                    ← ESP32-C6-specific port files
│   │   │   ├── tx_initialize_low_level.S   ← vector table, timer setup
│   │   │   ├── tx_esp32c6_timer.c          ← SYSTIMER + PLIC configuration
│   │   │   └── tx_port_startup.c           ← port_start_app_hook, tx_application_define
│   │   └── threadx/                 ← git submodule (upstream Azure RTOS ThreadX)
│   │       ├── common/src/          ← kernel sources
│   │       ├── ports/risc-v64/gnu/  ← RISC-V port assembly
│   │       └── utility/rtos_compatibility_layers/FreeRTOS/  ← compat layer
│   │
│   └── freertos_compat/             ← FreeRTOS API shim on top of ThreadX
│       ├── CMakeLists.txt           ← early-return guard when FREERTOS selected
│       ├── include/                 ← FreeRTOSConfig.h, portmacro.h overrides
│       └── src/port.c               ← newlib lock bridges, ESP-IDF integration
│
├── examples/
│   ├── threadx_demo/                ← native ThreadX API application
│   │   ├── CMakeLists.txt           ← EXTRA_COMPONENT_DIRS → ../../components/
│   │   ├── sdkconfig.defaults       ← CONFIG_RTOS_SELECTION_THREADX=y
│   │   └── main/
│   │       ├── CMakeLists.txt       ← REQUIRES threadx freertos_compat
│   │       └── main.c               ← uses tx_thread_create, tx_thread_sleep, etc.
│   │
│   └── freertos_demo/               ← FreeRTOS API application
│       ├── CMakeLists.txt           ← EXTRA_COMPONENT_DIRS → ../../components/
│       ├── sdkconfig.defaults       ← CONFIG_RTOS_SELECTION_FREERTOS=y (default)
│       └── main/
│           ├── CMakeLists.txt       ← conditional REQUIRES freertos_compat
│           └── main.c               ← uses xTaskCreate, vTaskDelay, etc.
│                                       works unchanged with EITHER RTOS selected
│
└── main/                            ← root project stub (placeholder only)
    ├── CMakeLists.txt
    └── main.c                       ← redirects user to examples/
```

### How the RTOS Selection Propagates Through the Build

When you run `idf.py menuconfig` from within an example directory and select
an RTOS, the following chain happens:

```
menuconfig writes → sdkconfig
sdkconfig read by → ESP-IDF cmake → sdkconfig.cmake (sets CONFIG_* CMake variables)
sdkconfig.cmake sets → CONFIG_RTOS_SELECTION_THREADX (or FREERTOS)
CMakeLists.txt reads → CONFIG_RTOS_SELECTION_THREADX in if() guards
  → threadx/CMakeLists.txt:    if(NOT CONFIG_RTOS_SELECTION_THREADX) → early return or full build
  → freertos_compat/CMakeLists.txt: same
  → main/CMakeLists.txt (freertos_demo): conditional REQUIRES
sdkconfig.h (compile flag -include sdkconfig.h) → makes CONFIG_* available in C/C++
  → tx_user.h reads CONFIG_THREADX_TICK_RATE_HZ, CONFIG_THREADX_MAX_PRIORITIES
  → tx_port_startup.c reads CONFIG_THREADX_BYTE_POOL_SIZE
```

Each example has its own independent `sdkconfig` file (created in its own
`build/` subdirectory). Changing RTOS selection in `examples/threadx_demo`
has no effect on `examples/freertos_demo`.

### Upgrade Path: From EXTRA_COMPONENT_DIRS to Managed Components

The current structure uses `EXTRA_COMPONENT_DIRS` in each example's top-level
`CMakeLists.txt` to reference the shared components. This is an interim approach
suitable for a monorepo layout.

The future goal is to publish `threadx` and `freertos_compat` as proper
**ESP-IDF managed components** (via the IDF Component Manager). Once published:

```yaml
# idf_component.yml — replaces EXTRA_COMPONENT_DIRS in each example
dependencies:
  threadx:
    version: ">=1.0.0"
  freertos_compat:
    version: ">=1.0.0"
```

With managed components, any ESP-IDF project anywhere — not just within this
repository — could add ThreadX support by adding the `idf_component.yml` file.
The `EXTRA_COMPONENT_DIRS` lines in each `CMakeLists.txt` would be deleted,
and no repository structure constraints would be imposed on users.

---

## 6. Interrupt Flow at Runtime

```
SYSTIMER comparator 0 fires every 160,000 ticks (10 ms at 16 MHz)
  │
  ▼
INTMTX maps source 57 → CPU interrupt line 7
  │
  ▼
PLIC MX (0x20001000) sees line 7 pending, priority=1 > threshold=0
  │
  ▼
CPU raises machine external interrupt (mip.MEIP set)
mstatus.MIE=1 AND mie.MEIE=1 → interrupt taken
  │
  ▼
CPU jumps to mtvec = _tx_esp32c6_trap_handler
  │
  ├─> _tx_thread_context_save()  — saves current thread registers
  ├─> reads PLIC_MX_EIP_STATUS (0x2000100C) — confirms line 7 set
  ├─> calls _tx_esp32c6_timer_isr()
  │     ├─> clears SYSTIMER INT (SYSTIMER_INT_CLR_REG)
  │     ├─> clears PLIC edge latch (PLIC_MX_CLEAR_REG, 0x20001008)
  │     └─> calls _tx_timer_interrupt()  — ThreadX tick processing
  │           └─> wakes sleeping threads, runs timer callbacks
  └─> _tx_thread_context_restore() — switches to highest-priority ready thread
        └─> mret  (returns to thread, restoring mstatus.MIE)
```
