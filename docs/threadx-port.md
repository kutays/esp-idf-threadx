# ThreadX Port Details

## Why risc-v64/gnu and Not risc-v32

The eclipse-threadx/threadx repository contains these RISC-V ports:

```
threadx/ports/
├── risc-v32/
│   ├── IAR/          ← Only IAR compiler, no GNU/GCC
│   └── (no gnu/)     ← Missing!
└── risc-v64/
    ├── gnu/          ← GNU/GCC toolchain ✓
    └── IAR/
```

There is no `risc-v32/gnu/` port. The ESP32-C6 uses GCC (via ESP-IDF).
Therefore we use `risc-v64/gnu/` which is written to work for both 32 and 64-bit
RISC-V via the STORE/LOAD/REGBYTES macro abstraction in `tx_port.h`.

The relevant section of `ports/risc-v64/gnu/inc/tx_port.h`:
```c
#if __riscv_xlen == 64
    #define STORE    sd
    #define LOAD     ld
    #define REGBYTES 8
#else
    #define STORE    sw
    #define LOAD     lw
    #define REGBYTES 4
#endif
```

The ESP-IDF GCC toolchain for ESP32-C6 sets `__riscv_xlen = 32`, so all the
assembly uses 32-bit stores/loads and 4-byte register slots.

## Files from the Submodule (used unchanged)

```
threadx/common/src/*.c                              — all ~80 kernel C files
threadx/common/inc/tx_api.h                         — public API
threadx/common/inc/tx_port.h                        — platform abstraction
threadx/ports/risc-v64/gnu/inc/tx_port.h            — RISC-V port header
threadx/ports/risc-v64/gnu/src/tx_thread_context_save.S
threadx/ports/risc-v64/gnu/src/tx_thread_context_restore.S
threadx/ports/risc-v64/gnu/src/tx_thread_schedule.S
threadx/ports/risc-v64/gnu/src/tx_thread_stack_build.S
threadx/ports/risc-v64/gnu/src/tx_thread_system_return.S
threadx/ports/risc-v64/gnu/src/tx_thread_interrupt_control.S
```

## Files We Override / Replace

```
threadx/ports/risc-v64/gnu/src/tx_initialize_low_level.S
  → Replaced by: components/threadx/port/tx_initialize_low_level.S
  → Reason: ESP32-C6 needs SYSTIMER (not mtime) and PLIC MX (not standard PLIC)

threadx/ports/risc-v64/gnu/src/tx_timer_interrupt.S
  → Replaced by: components/threadx/port/tx_timer_interrupt.c
  → Reason: ESP32-C6 SYSTIMER is completely different from mtime/mtimecmp
```

The CMakeLists.txt explicitly lists the upstream port assembly files to include,
and adds our override files. The upstream `tx_initialize_low_level.S` and
`tx_timer_interrupt.S` are NOT listed, so they are silently skipped.

## tx_user.h — Compile-Time Configuration

Location: `components/threadx/include/tx_user.h`

ThreadX picks this up when `TX_INCLUDE_USER_DEFINE_FILE` is defined (set in
CMakeLists.txt).

```c
#define TX_TIMER_TICKS_PER_SECOND   100
```
100 Hz tick rate. Each call to `tx_thread_sleep(N)` sleeps for N/100 seconds.
Used in `tx_timer_interrupt.c` to compute SYSTIMER_CLK_FREQ / 100 = 160,000
ticks per alarm period.

```c
#define TX_MAX_PRIORITIES           32
```
Threads have priorities 0 (highest) through 31 (lowest). The main thread uses
priority 16, blink thread uses priority 5 (higher than main).

```c
#define TX_ENABLE_STACK_CHECKING
```
ThreadX checks for stack overflow on each context switch. Threads are given a
water mark at the bottom of their stack; if the water mark is overwritten, the
`_tx_thread_stack_error_handler` is called.

```c
#define TX_THREAD_USER_EXTENSION    VOID *txfr_thread_ptr;
```
This is required by the FreeRTOS compatibility layer (`tx_freertos.c`). It adds
a field to every `TX_THREAD` control block that the compat layer uses to find
its wrapper structure. Without this, `tx_freertos.c` will fail to compile with
"'txfr_thread_ptr' has no member in TX_THREAD".

```c
#define TX_PORT_USE_CUSTOM_TIMER        1
#define TX_PORT_SPECIFIC_PRE_SCHEDULER_INITIALIZATION
```
These are hints to the build system and to our port code that we are using a
custom timer (SYSTIMER) instead of the standard RISC-V mtime/mtimecmp. The
actual effect is documentation — our CMakeLists.txt does not include
`tx_timer_interrupt.S` from the upstream port, so the standard timer ISR
is simply absent.

## Kconfig Options

`components/threadx/Kconfig` exposes these options in `idf.py menuconfig`:

| Option                    | Default | Description                    |
|--------------------------|---------|--------------------------------|
| THREADX_TICK_RATE_HZ     | 100     | Tick frequency (Hz)            |
| THREADX_MAX_PRIORITIES   | 32      | Max priority levels            |
| THREADX_BYTE_POOL_SIZE   | 32768   | System byte pool size          |
| THREADX_TIMER_CPU_INT    | 7       | CPU interrupt line for SYSTIMER|

Note: The Kconfig values are not automatically fed into `tx_user.h` — they are
separate. `tx_user.h` has hardcoded values that must match `sdkconfig.defaults`.
For a production build, these should be unified.

## Linker Fragment (linker.lf)

```
[mapping:threadx]
archive: libthreadx.a
entries:
    tx_thread_schedule (noflash_text)
    tx_thread_context_save (noflash_text)
    tx_thread_context_restore (noflash_text)
    tx_thread_system_return (noflash_text)
    tx_initialize_low_level (noflash_text)
    tx_timer_interrupt (noflash_text)
```

`noflash_text` tells the ESP-IDF linker to place these object files in IRAM
(internal RAM, mapped at 0x40800000) instead of flash.

**Why this matters**: On ESP32-C6, code normally executes from flash via a
cache. Cache misses can take dozens of cycles. During an interrupt, cache misses
in the ISR handler add latency and jitter. By placing the context save/restore,
scheduler, and timer ISR in IRAM, these paths execute at full RAM speed with
deterministic latency.

The tradeoff: IRAM is limited (~400 KB on ESP32-C6). These ThreadX files are
small (a few KB), so it's a good investment.

## System Byte Pool

ThreadX uses a **byte pool** (variable-size heap) for dynamic allocations.
We create a 32 KB pool in `tx_application_define()` from a static array
(`system_byte_pool_storage[32768]`).

The FreeRTOS compat layer also needs this pool for `pvPortMalloc` / `xTaskCreate`
allocations.

`_tx_esp32c6_system_byte_pool` is a global pointer to the pool, available for
use by any component that needs dynamic memory.

## _tx_timer_interrupt — The ThreadX Tick Handler

This is an internal ThreadX function defined in `threadx/common/src/tx_timer_interrupt.c`.
It is NOT the same as our `_tx_esp32c6_timer_isr`.

Call chain:
```
SYSTIMER fires
  → _tx_esp32c6_trap_handler  (assembly, our file)
    → _tx_esp32c6_timer_isr   (C, our file)
      → _tx_timer_interrupt   (C, ThreadX kernel)
        → decrements all sleeping thread sleep counters
        → if any counter reaches 0: moves thread to Ready list
        → runs any expired tx_timer callbacks
```

`_tx_timer_interrupt` is not declared in any public ThreadX header. We add:
```c
extern VOID _tx_timer_interrupt(VOID);
```
at the top of `tx_timer_interrupt.c`.
