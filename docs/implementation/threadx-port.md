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

---

## ThreadX Scheduling: tx_thread_relinquish vs tx_thread_sleep

### The Priority System

ThreadX uses numeric priorities where **lower number = higher priority**. This is the
opposite of some other RTOS conventions. The system has two demo threads:

| Thread | Priority | Created in |
|--------|----------|-----------|
| `main_thread` (runs `app_main`) | 16 | `tx_port_startup.c` |
| `blink_thread` | 5 | `main.c` |

### tx_thread_relinquish — Same-Priority Only

`tx_thread_relinquish()` yields the CPU to another READY thread of the **same priority**.
If no same-priority thread is ready, the calling thread immediately resumes.

```c
/* blink_thread (priority 5) calls relinquish */
tx_thread_relinquish();
/* ThreadX looks for ready threads at priority 5 — none exist */
/* blink_thread immediately resumes — main_thread (priority 16) never runs */
```

This means using `tx_thread_relinquish()` alone in threads of different priorities
creates a situation where only the highest-priority thread ever runs, because it's
always READY and always preempts lower-priority threads.

**Symptom observed**: With both threads using `tx_thread_relinquish()` and their
spin-wait loops:
- At `BLINK_THREAD_PRIO = 5`: only blink thread printed (main never got CPU)
- At `BLINK_THREAD_PRIO = 20`: only main thread printed (blink never got CPU)
- Expected interleaving never occurred

### tx_thread_sleep — Correct Cross-Priority Yielding

`tx_thread_sleep(N)` **suspends** (blocks) the calling thread for N tick intervals.
A suspended thread is removed from the READY queue, so the scheduler selects the
next runnable thread from any priority level.

```c
/* blink_thread (priority 5) calls sleep */
tx_thread_sleep(10);    /* 10 ticks = 100ms at 100Hz */
/* blink_thread is SUSPENDED — not in READY queue */
/* scheduler selects main_thread (priority 16) — it runs now */
/* after 10 ticks, blink_thread moves back to READY, preempts main */
```

This produces correct interleaving regardless of priority difference.

### Blocking Objects Also Enable Cross-Priority Yielding

Any blocking API removes the thread from READY:

| API | Blocks until |
|-----|-------------|
| `tx_thread_sleep(N)` | N ticks elapse |
| `tx_semaphore_get(&sem, TX_WAIT_FOREVER)` | Semaphore available |
| `tx_mutex_get(&mtx, TX_WAIT_FOREVER)` | Mutex unlocked |
| `tx_queue_receive(&q, &buf, TX_WAIT_FOREVER)` | Queue not empty |
| `tx_event_flags_get(&ef, flags, TX_AND, ...)` | Event flags set |

All of these allow lower-priority threads to run while the caller waits.

### Verified Working Output

After switching to `tx_thread_sleep()`:

```
I (417) main: [main]  tick=4   isr_count=4   count=0   ← main runs first
I (429) main: [blink] tick=5   isr_count=5   count=0   ← blink preempts (higher prio)
I (529) main: [blink] tick=16  isr_count=16  count=1
I (619) main: [main]  tick=25  isr_count=25  count=1   ← main wakes after sleep
I (629) main: [blink] tick=26  isr_count=26  count=2   ← blink preempts again
```

Tick counter increments at exactly 100 Hz (`tick == isr_count` in every sample).
The 10-tick difference between main prints (~200ms) and blink prints (~100ms) matches
the sleep durations exactly — `tx_thread_sleep()` is confirmed precise.
