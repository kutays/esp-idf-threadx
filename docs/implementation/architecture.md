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
