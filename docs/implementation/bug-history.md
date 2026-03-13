# Bug History and Investigation Log

This file documents every bug encountered during implementation, the symptoms,
root cause, and the fix applied. This is the authoritative record of what we
tried and why the current code looks the way it does.

---

## Bug 1: SYSTIMER_TARGET0_PERIOD_REG Undeclared

**Phase**: First build attempt

**Symptom**: Compiler error:
```
tx_timer_interrupt.c: error: 'SYSTIMER_TARGET0_PERIOD_REG' undeclared
```

**Wrong assumption**: We assumed there was a separate register to write the
period value, like `SYSTIMER_TARGET0_PERIOD_REG`.

**Root cause**: The SYSTIMER has no separate period register. The period is
a bitfield within `SYSTIMER_TARGET0_CONF_REG` at bits [25:0]. The entire
configuration (period value, period mode, unit select) lives in one register.

**Fix**:
```c
// Wrong:
write_reg(SYSTIMER_TARGET0_PERIOD_REG, period);
write_reg(SYSTIMER_TARGET0_CONF_REG, SYSTIMER_TARGET0_PERIOD_MODE);

// Correct:
write_reg(SYSTIMER_TARGET0_CONF_REG,
          (period & SYSTIMER_TARGET0_PERIOD_V) | SYSTIMER_TARGET0_PERIOD_MODE);
```

**References**: `soc/esp32c6/register/soc/systimer_reg.h`, SYSTIMER_TARGET0_CONF_REG.

---

## Bug 2: _tx_timer_interrupt Implicit Declaration

**Phase**: First build attempt

**Symptom**: Compiler warning/error:
```
tx_timer_interrupt.c: implicit declaration of function '_tx_timer_interrupt'
```

**Root cause**: `_tx_timer_interrupt` is the internal ThreadX tick handler
defined in `threadx/common/src/tx_timer_interrupt.c`. It is not declared in
any public ThreadX header (`tx_api.h`, `tx_timer.h`, etc.).

**Fix**: Add an explicit extern declaration at the top of our file:
```c
extern VOID _tx_timer_interrupt(VOID);
```

---

## Bug 3: TX_DISABLE — interrupt_save Undeclared

**Phase**: Second build (freertos_compat/src/port.c)

**Symptom**:
```
port.c: error: 'interrupt_save' undeclared
```

**Root cause**: The `TX_DISABLE` macro expands to code that references
`interrupt_save`, but this variable must be declared by the `TX_INTERRUPT_SAVE_AREA`
macro first. We had `TX_DISABLE` without `TX_INTERRUPT_SAVE_AREA`.

**Fix**: Always pair them:
```c
void vPortEnterCritical(void)
{
    TX_INTERRUPT_SAVE_AREA   // declares: ULONG64 interrupt_save;
    TX_DISABLE               // uses interrupt_save
    ...
}
```

---

## Bug 4: _lock_t Conflicting Types

**Phase**: Second build (freertos_compat/src/port.c)

**Symptom**:
```
port.c: error: conflicting types for '_lock_t'
```

**Root cause**: We tried to define our own `_lock_t` type, but ESP-IDF's newlib
already defines it in `<sys/lock.h>`:
```c
typedef struct __lock * _lock_t;
struct __lock { int reserved[21]; };
```

**Fix**: Use ESP-IDF's existing definition. Store a `TX_MUTEX` inside the
`reserved[21]` array (which is 84 bytes — large enough for TX_MUTEX at ~76 bytes):
```c
#include <sys/lock.h>
static TX_MUTEX *lock_to_mutex(struct __lock *lock) {
    return (TX_MUTEX *)lock->reserved;
}
```

---

## Bug 5: TX_MUTEX_ID Undeclared

**Phase**: Second build

**Symptom**:
```
port.c: error: 'TX_MUTEX_ID' undeclared
```

**Root cause**: We tried to check if a lock had been initialized by comparing
against `TX_MUTEX_ID`. This constant does not exist in the ThreadX API.

**Fix**: Remove the check entirely. Use `if (plock && *plock)` to guard against
null pointers, and trust that `tx_mutex_create` initializes the structure properly.

---

## Bug 6: FreeRTOS Type Conflicts (BaseType_t, StackType_t, etc.)

**Phase**: Third build (after switching to upstream tx_freertos.c)

**Symptom**:
```
error: conflicting types for 'BaseType_t'
error: 'UBaseType_t' declared as different type
error: conflicting types for 'StackType_t'
```

**Root cause**: Our original `FreeRTOSConfig.h` included `portmacro.h`, which
included ESP-IDF's real FreeRTOS headers via the include chain:
```
FreeRTOSConfig.h → portmacro.h → freertos/portmacro.h (ESP-IDF real FreeRTOS)
```
That pulled in the real FreeRTOS type definitions, which conflicted with those
in the upstream ThreadX compat layer's `FreeRTOS.h`.

**Fix**: Remove `#include "portmacro.h"` from `FreeRTOSConfig.h`. Define the
port macros (portYIELD, portDISABLE_INTERRUPTS, etc.) directly as inline
expansions in `FreeRTOSConfig.h` without including any portmacro.h.

---

## Bug 7: configTOTAL_HEAP_SIZE — _heap_end Undeclared

**Phase**: Third build

**Symptom**:
```
error: '_heap_end' undeclared
error: '_heap_start' undeclared
```

**Root cause**: Some code path was reaching ESP-IDF's real `FreeRTOSConfig.h`,
which defines `configTOTAL_HEAP_SIZE` using ESP-IDF linker symbols
(`_heap_end - _heap_start`). The upstream compat `FreeRTOS.h` was not
finding our config first.

**Fix**: Add `target_include_directories(... BEFORE PUBLIC ...)` in
`freertos_compat/CMakeLists.txt` to ensure our `FreeRTOSConfig.h` is found
before any ESP-IDF FreeRTOS config. Also define:
```c
#define configTOTAL_HEAP_SIZE   (1024u * 32u)  // simple constant
```

---

## Bug 8: txfr_thread_ptr Has No Member in TX_THREAD

**Phase**: Third build (tx_freertos.c compilation)

**Symptom**:
```
tx_freertos.c: error: 'TX_THREAD' has no member named 'txfr_thread_ptr'
```

**Root cause**: The upstream `tx_freertos.c` uses a back-pointer field called
`txfr_thread_ptr` inside every `TX_THREAD` control block to link to the
FreeRTOS task wrapper struct. This field must be added via the user-extension
mechanism in `tx_user.h`.

**Fix**: Add to `tx_user.h`:
```c
#define TX_THREAD_USER_EXTENSION    VOID *txfr_thread_ptr;
```
ThreadX's `TX_THREAD` struct includes `TX_THREAD_USER_EXTENSION` if defined,
inserting the field at a specific offset.

---

## Bug 9: TX_VERSION_ID Undeclared

**Phase**: Fourth build (main.c)

**Symptom**:
```
main.c: error: 'TX_VERSION_ID' undeclared
```

**Root cause**: We used `TX_VERSION_ID` which does not exist in the ThreadX
public API.

**Fix**: Use `THREADX_MAJOR_VERSION` which is defined in `tx_api.h`:
```c
ESP_LOGI(TAG, "ThreadX version: %u", (unsigned)THREADX_MAJOR_VERSION);
```

---

## Bug 10: tick=0 — tx_thread_sleep Has No Effect

**Phase**: First hardware run (system boots and runs, threads print, but tick
stays at 0 and output floods at full speed)

**Symptom**: Both threads print without any delay. `tx_time_get()` returns 0
indefinitely.

**Root cause (first layer)**: The trap handler calling convention was wrong.
`_tx_thread_context_save` requires:
1. The interrupt stack frame already allocated on the stack (`sp` decremented)
2. `x1` (ra) already saved at `28*REGBYTES(sp)`

Our original handler called `_tx_thread_context_save` without allocating the
frame or saving `ra` first. The context save code read garbage from the wrong
stack offsets.

**Fix (first layer)**:
```asm
_tx_esp32c6_trap_handler:
    addi    sp, sp, -INT_FRAME_SIZE    // allocate 128-byte frame
    STORE   x1, 28*REGBYTES(sp)        // save ra before call overwrites it
    call    _tx_thread_context_save
```

Even after this fix, tick remained 0. More bugs below.

---

## Bug 11: Wrong SYSTIMER Interrupt Source Number (37 vs 57)

**Phase**: After bug 10 fix

**Root cause**: We hardcoded `SYSTIMER_TARGET0_INT_SOURCE = 37`. The correct
value is **57**.

Verified by counting in `soc/esp32c6/include/soc/interrupts.h`:
```c
typedef enum {
    ETS_WIFI_MAC_INTR_SOURCE = 0,
    ...
    ETS_SYSTIMER_TARGET0_INTR_SOURCE = 57,  // ← correct
    ...
} esp_intr_source_t;
```

**Effect of wrong value**: We were programming the INTMTX map register for
source 37 (some other peripheral) to CPU line 7. Source 57 (SYSTIMER_TARGET0)
was still mapped to whatever it defaulted to (probably CPU line 0 or unmapped).

**Fix**: `#define SYSTIMER_TARGET0_INT_SOURCE  57`

---

## Bug 12: Wrong INTMTX Map Register Formula

**Phase**: After bug 11 fix

**Wrong formula**: `INTMTX_BASE + 0x80 + src * 4`

**Correct formula**: `INTMTX_BASE + src * 4`

**Root cause**: We guessed an 0x80 base offset for the map registers. The actual
register layout starts at offset 0 from the INTMTX base address.

**Verification**: `soc/esp32c6/register/soc/interrupt_matrix_reg.h` shows:
```c
#define INTMTX_CORE0_WIFI_MAC_INTR_MAP_REG  (DR_REG_INTERRUPT_MATRIX_BASE + 0x0)
// source 0 is at offset 0x0, so source N is at offset N*4
```

**Effect**: We were writing to a wrong register (offset 0x80 + 57*4 = 0x1C4
instead of 57*4 = 0xE4). The SYSTIMER source remained unmapped.

---

## Bug 13: Wrong INTPRI Base Address

**Phase**: After bug 12 fix

**Wrong base**: `0x60010000` (this is INTMTX, not INTPRI)

**Correct base**: `DR_REG_INTPRI_BASE = 0x600C5000`

**Root cause**: We confused the two registers. Both live on the main APB bus
at 0x6xxxxxxx but at different offsets.

**Fix**: Use `DR_REG_INTPRI_BASE` from `soc/reg_base.h`.

---

## Bug 14: Wrong EIP_STATUS Address in Assembly

**Phase**: After bug 13 fix

**Wrong address**: `0x60010048` (some unknown register)

**Correct address**: `0x600C5008` (INTPRI_CORE0_CPU_INT_EIP_STATUS_REG)

**Root cause**: We had a stale hardcoded address in the assembly from an earlier
incorrect guess.

**Fix**: Updated `.equ INTPRI_EIP_STATUS_REG, 0x600C5008` in the assembly.

After all these fixes, tick still remained 0. The interrupt was still not firing.

---

## Bug 15: Wrong Interrupt Controller — INTPRI vs PLIC MX

**Phase**: After bugs 10-14 were fixed

**Root cause**: All along we were configuring the wrong interrupt controller.

The ESP32-C6 has `SOC_INT_PLIC_SUPPORTED=y`. There are two separate controllers:
- **INTPRI** at `0x600C5000` — Espressif's custom peripheral on main APB bus
- **PLIC MX** at `0x20001000` — The actual machine-mode CPU interrupt controller

For machine-mode code (which is what ThreadX runs as), CPU interrupt lines
must be configured via PLIC MX registers at 0x20001000, NOT INTPRI at 0x600C5000.

**Evidence**: Found in `soc/esp32c6/register/soc/plic_reg.h`:
- `PLIC_MXINT_ENABLE_REG = 0x20001000`
- `PLIC_MXINT_TYPE_REG = 0x20001004`
- `PLIC_MXINT_CLEAR_REG = 0x20001008`
- `PLIC_EMIP_STATUS_REG = 0x2000100C`
- `PLIC_MXINT0_PRI_REG = 0x20001010`

The INTPRI might be an alias for user-mode operation or legacy code. For machine
mode, PLIC MX is authoritative.

**Also**: The EIP_STATUS we read in the trap handler assembly must use the PLIC
address (0x2000100C), not the INTPRI address (0x600C5008).

**Fix**:
1. Changed `tx_timer_interrupt.c` to use PLIC_MX_* registers
2. Changed assembly `.equ PLIC_MX_EIP_STATUS_REG, 0x2000100C`
3. Changed ISR clear to use `PLIC_MX_CLEAR_REG = 0x20001008`

---

## Bug 16: mie.MEIE Not Set

**Phase**: Identified alongside bug 15

**Root cause**: ThreadX's port manages `mstatus.MIE` (the global interrupt
enable) but does not set `mie.MEIE` (the machine external interrupt enable
bit 11). Without `mie.MEIE = 1`, the CPU will not accept any external interrupt
from the PLIC, even if `mstatus.MIE = 1` and the PLIC is fully configured.

The RISC-V spec requires both:
```
mstatus.MIE = 1   (global gate)
mie.MEIE = 1      (external interrupt gate)
```

**Fix**: Added to `_tx_initialize_low_level`, before calling timer setup:
```asm
li    t0, 0x800      // bit 11 = MEIE
csrs  mie, t0        // set MEIE in mie CSR
```

`0x800 = 0b100000000000` — bit 11 of mie.

---

## Bug 17: SYSTIMER_TARGET0_WORK_EN Not Set (bit 24 of SYSTIMER_CONF_REG)

**Phase**: Identified during documentation review of SYSTIMER register map

**Root cause**: Even with SYSTIMER configured and PLIC set up, the SYSTIMER
comparator 0 will not generate an interrupt unless bit 24 of `SYSTIMER_CONF_REG`
is set. This bit is called `SYSTIMER_TARGET0_WORK_EN`.

We only set bit 0 (CLK_FO = force clock on) but forgot bit 24.

```
SYSTIMER_CONF_REG bits:
  bit  0: CLK_FO — force systimer clock on
  bit 24: TARGET0_WORK_EN — enable comparator 0 to fire   ← was missing
  bit 30: TIMER_UNIT0_WORK_EN — enable counter unit 0 (default=1)
```

**Fix**:
```c
// Wrong:
write_reg(SYSTIMER_CONF_REG, read_reg(SYSTIMER_CONF_REG) | (1 << 0));

// Correct:
write_reg(SYSTIMER_CONF_REG,
          read_reg(SYSTIMER_CONF_REG) | (1 << 0) | (1 << 24));
```

---

## Bug 18: SYSTIMER_TARGET0_CONF_REG Bit 31 Incorrectly Set

**Phase**: Identified during documentation review

**Root cause**: In an earlier version of the code we had:
```c
write_reg(SYSTIMER_TARGET0_CONF_REG,
          read_reg(SYSTIMER_TARGET0_CONF_REG) | (1u << 31));
```

We thought bit 31 was an "enable" bit. It is actually `TIMER_UNIT_SEL`:
- Bit 31 = 0: comparator 0 uses counter unit 0 (correct)
- Bit 31 = 1: comparator 0 uses counter unit 1 (wrong — unit 1 is separate)

Setting bit 31 = 1 means COMP0 watches unit 1's counter, but we only started
unit 0. Unit 1 might be stopped or counting at a different rate, so COMP0
never fires.

**Fix**: Removed the `| (1u << 31)` write entirely. The correct setup writes
only period + PERIOD_MODE (bit 30):
```c
write_reg(SYSTIMER_TARGET0_CONF_REG,
          (TIMER_ALARM_PERIOD & SYSTIMER_TARGET0_PERIOD_V) |
          SYSTIMER_TARGET0_PERIOD_MODE);  // bit 30 only
```

---

---

## Bug 19: `esp_startup_start_app` Is a Strong Symbol — Cannot Override

**Phase**: First hardware run showing `main_task` in output instead of `threadx_startup`

**Symptom**: Serial output shows:
```
I (252) main_task: Started on CPU0
I (252) main_task: Calling app_main()
```
This is FreeRTOS's own `main_task` tag from `app_startup.c`, not our ThreadX
startup code. ThreadX is never initialized. Every ThreadX call runs against
zeroed BSS:
- `tx_thread_sleep()` → `_tx_thread_current_ptr == NULL` → returns `TX_CALLER_ERROR`
  immediately, the return value is ignored, loop floods at full speed
- `tx_time_get()` → reads `_tx_timer_system_clock` which is 0 (BSS) → always 0
- `tx_thread_create()` → `_tx_initialize_remaining_to_complete` is 0 (BSS) →
  passes the "is initialized?" check accidentally, creates a broken thread

**Wrong assumption**: We declared `void esp_startup_start_app(void)` as a
strong symbol in `tx_port_startup.c` expecting it to override FreeRTOS's weak
version. But it is **not weak** in the current ESP-IDF:

```c
// components/freertos/app_startup.c — line 64, NO __attribute__((weak)):
void esp_startup_start_app(void)   // ← STRONG symbol
{
    xTaskCreatePinnedToCore(main_task, ...);
    if (port_start_app_hook != NULL)
        port_start_app_hook();         // ← THIS is the weak hook
    vTaskStartScheduler();
}
```

Two strong definitions of the same symbol is a linker error, so the linker
silently preferred FreeRTOS's version and never extracted our object file from
the archive, because weak symbols do not trigger archive searches.

**Fix**: Use the `port_start_app_hook` weak hook that FreeRTOS calls just
before `vTaskStartScheduler()`. If our strong `port_start_app_hook()` calls
`tx_kernel_enter()` which never returns, `vTaskStartScheduler()` is never
reached and FreeRTOS never starts:

```c
// tx_port_startup.c
void port_start_app_hook(void)   // strong — overrides weak declaration
{
    tx_kernel_enter();           // never returns → FreeRTOS scheduler never starts
}
```

The FreeRTOS `main_task` is created before this hook is called but never gets
scheduled because the FreeRTOS scheduler never starts.

---

## Bug 20: `port_start_app_hook` Not Linked In (Weak Symbol Archive Problem)

**Phase**: After bug 19 fix — still seeing `main_task` messages

**Symptom**: Identical to bug 19. Same output. Linker finds FreeRTOS's null
(weak) resolution for `port_start_app_hook` before ever searching our archive.

**Root cause**: The GNU linker only searches archive libraries (`.a` files)
for symbols that are currently **undefined and strong**. A weak symbol is
always satisfiable by a null reference — the linker never goes looking for a
stronger definition in an archive. Our `port_start_app_hook` strong definition
sits in `libthreadx.a` but the linker never opens the archive to find it.

This is a fundamental property of static linking with archive files:
> **Weak symbols never trigger archive extraction.**

**Fix**: Add `-Wl,-u,port_start_app_hook` to the component's link options.
`-u symbol` tells the linker: "treat this symbol as undefined even if it's
not referenced; you must resolve it." This forces the linker to search archives
for our strong definition:

```cmake
# components/threadx/CMakeLists.txt
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,-u,port_start_app_hook")
```

`INTERFACE` propagates the flag to anything that links against the threadx
component, which includes the final executable.

---

## Bug 21: ESP32-C6 MTVEC Always Uses Vectored Dispatch

**Phase**: ThreadX now starting (port_start_app_hook working), but blink thread
never prints, tick=0, timer interrupt never reaches our handler

**Symptom**: Timer interrupt never fires. The `_tx_esp32c6_trap_handler` is
never entered despite the SYSTIMER being configured.

**Root cause**: We installed mtvec by pointing it directly at our handler:
```asm
la    t0, _tx_esp32c6_trap_handler
csrw  mtvec, t0                      // WRONG for ESP32-C6
```

The standard RISC-V spec has two mtvec modes:
- Mode 00 (Direct): all traps jump to `mtvec_base`
- Mode 01 (Vectored): interrupts jump to `mtvec_base + cause * 4`

We assumed mode 00 (direct). But the ESP32-C6 hardware comment in
`components/riscv/vectors_intc.S` reveals:

```asm
/* Note: for our CPU, we need to place this on a 256-byte boundary, as CPU
 * only uses the 24 MSBs of the MTVEC, i.e. (MTVEC & 0xffffff00). */
.balign 0x100

/* The CPU jumps to MTVEC (i.e. the first entry) in case of an exception,
 * and (MTVEC & 0xfffffffc) + (mcause & 0x7fffffff) * 4, in case of interrupt */
_vector_table:
    j _panic_handler            /* 0: Exception entry */
    j _interrupt_handler        /* 1: CPU interrupt 1 */
    ...
    j _interrupt_handler        /* 7: CPU interrupt 7  ← our SYSTIMER */
```

The CPU **always** uses vectored dispatch regardless of the mode bits. For
CPU interrupt line 7 (our SYSTIMER):
```
mcause = 0x80000007  (bit 31 = interrupt, code = 7)
CPU jumps to: (mtvec & 0xffffff00) + 7 * 4 = mtvec_base + 28
```

With `mtvec = _tx_esp32c6_trap_handler` (say at address `0x42001234`):
- `mtvec_base = 0x42001200` (rounded down to 256-byte boundary)
- Interrupt 7 → CPU jumps to `0x42001200 + 28 = 0x4200121c`
- This is **28 bytes past our handler's entry point**, landing in the middle
  of the instruction stream — undefined behavior, likely a crash.

**Fix**: Create a proper 256-byte aligned vector table where each entry is
a 4-byte `j` instruction (one per CPU interrupt line):

```asm
    .section .iram0.text,"ax",@progbits
    .balign 256          // must be 256-byte aligned

    .option push
    .option norvc        // force 4-byte j instructions (not 2-byte c.j)

_tx_esp32c6_vector_table:
    j   _tx_esp32c6_exception_entry /* 0: CPU exception */
    j   _tx_esp32c6_unused_int      /* 1: unused */
    ...
    j   _tx_esp32c6_trap_handler    /* 7: SYSTIMER tick */
    ...

    .option pop
```

Then point mtvec at the table:
```asm
la    t0, _tx_esp32c6_vector_table
csrw  mtvec, t0
```

Why `.option norvc`: The RISC-V `C` extension (compressed instructions) allows
`j` to assemble as a 2-byte `c.j`. If that happens, entry 7 would be at
`base + 7*2 = base + 14` instead of `base + 7*4 = base + 28`, and the CPU's
dispatch (which always uses 4-byte spacing) would jump to the wrong entry.
`.option norvc` forces the assembler to emit 4-byte instructions in this region.

---

## Bug 22: Exception Handler Silently Spinning — Crashes Are Invisible

**Phase**: After vector table fix applied

**Symptom**: blink thread created (status=0) but never prints anything, not
even once. No crash output. System appears to hang silently.

**Root cause**: Our original exception handler was:
```asm
_handle_exception:
    j   _handle_exception     // spin forever
```

When blink_thread overflowed its stack on the first `ESP_LOGI` call (bug 23
below), a store-access-fault exception fired. The exception went to our handler
which silently spun. No panic dump, no log output, no indication of any problem.

**Fix**: Replace the spin with a call to `abort()` after switching to a known
safe stack. We switch to the system stack first because the thread stack may
be corrupt (it was just overflowed):

```asm
_tx_esp32c6_exception_entry:
    /* Switch to system stack — thread stack may be corrupt (e.g. overflowed) */
    la      t0, _tx_thread_system_stack_ptr
    LOAD    sp, 0(t0)
    call    abort        /* noreturn — triggers ESP-IDF panic handler with crash dump */
```

This gives a proper ESP-IDF panic output showing the faulting address and
register state, making crashes debuggable.

---

## Bug 23: blink_thread Stack Too Small — Overflow Before First Print

**Phase**: After vector table fix, exception handler now calls abort()

**Symptom**: Panic dump from abort() triggered by an exception in blink_thread
before the first `ESP_LOGI` message ever appeared.

**Root cause**: `BLINK_STACK_SIZE` was 2048 bytes. `ESP_LOGI` is deeply stack-
consuming on ESP-IDF:

```
blink_thread_entry → ESP_LOGI → esp_log_write → esp_log_vprintf
  → vprintf → UART formatting + write chain
```

This call chain uses more than 2048 bytes on a debug build, causing a stack
overflow on the very first log call.

The main_thread uses 4096 bytes and successfully calls `ESP_LOGI`, so the
minimum safe size for any thread that uses ESP logging is at least 4096 bytes.

**Fix**: Increase `BLINK_STACK_SIZE` from 2048 → 4096 bytes in `main.c`.

---

## Bug 24: R_RISCV_JAL Relocation Truncated — IRAM to Flash Distance > 1MB

**Phase**: Build error after vector table added

**Linker error**:
```
relocation truncated to fit: R_RISCV_JAL against `_tx_esp32c6_trap_handler'
relocation truncated to fit: R_RISCV_JAL against `_tx_esp32c6_exception_entry'
```

**Root cause**: Two memory regions involved:
- Vector table: in `.iram0.text` → placed in IRAM at ~`0x40800000`
- Trap handler: in `.text` → placed in flash cache region at ~`0x42000000`

Distance ≈ **32 MB**.

The `j` instruction in RISC-V is `jal x0, offset` — a PC-relative jump with
a **signed 21-bit offset**, giving a range of ±1 MB. From IRAM to flash is
32× larger than that range.

The same problem applies to `j _tx_thread_context_restore` at the end of the
trap handler: `_tx_thread_context_restore` is in `.text` (flash), and a `j`
from IRAM cannot reach it.

**Fix — part 1**: Move `_tx_esp32c6_trap_handler` to `.iram0.text` by adding
a section directive before its definition:

```asm
    .section .iram0.text,"ax",@progbits   // ← switch back to IRAM section
    .align 4
    .type _tx_esp32c6_trap_handler, @function
_tx_esp32c6_trap_handler:
    ...
```

Now all vector table entries (`j` instructions in IRAM) jump to targets also
in IRAM — well within ±1 MB.

**Fix — part 2**: At the end of the trap handler, replace `j` with `la + jr`
for the far jump to `_tx_thread_context_restore` (which remains in flash):

```asm
// Before (FAILS — JAL, ±1 MB):
j       _tx_thread_context_restore

// After (WORKS — register-indirect, unlimited range):
la      t0, _tx_thread_context_restore   // auipc + addi: 32-bit PC-relative, any address
jr      t0                               // jalr x0, t0, 0: register-indirect, any address
```

`la label` expands to two instructions:
- `auipc t0, label[31:12]` — loads upper 20 bits relative to PC
- `addi  t0, t0, label[11:0]` — adds lower 12 bits

This 2-instruction sequence can construct any 32-bit address regardless of
distance. `jr t0` is `jalr x0, t0, 0` — a register-indirect jump with no
range limit. The combination reaches any address in the 32-bit space.

`call` (used elsewhere in the handler) also has unlimited range because it
uses the same `auipc + jalr` sequence — the difference is that `call` saves
the return address in `ra`, while `la + jr` discards it (tail-call).

---

## Bug 25: Port C File `tx_timer_interrupt.c` Never Added to Build

**Phase**: Same build as bug 24

**Linker error**:
```
undefined reference to `_tx_timer_interrupt'
```

**Root cause**: `_tx_timer_interrupt()` is the internal ThreadX function that
increments the system tick and drives timer/time-slice expiration. It lives in
the port's C file:

```
ports/risc-v64/gnu/src/tx_timer_interrupt.c
```

Our `CMakeLists.txt` compiled the port's **assembly** files:

```cmake
set(THREADX_PORT_ASM
    .../tx_thread_context_save.S
    .../tx_thread_context_restore.S
    .../tx_thread_schedule.S
    .../tx_thread_stack_build.S
    .../tx_thread_system_return.S
    .../tx_thread_interrupt_control.S
)
```

But the port's C file was never listed. It is not part of the common ThreadX
sources (`common/src/`) — it is port-specific and must be added explicitly.

**Fix**: Add `THREADX_PORT_C` to the source list:

```cmake
set(THREADX_PORT_C
    ${THREADX_DIR}/ports/risc-v64/gnu/src/tx_timer_interrupt.c
)

idf_component_register(
    SRCS
        ${THREADX_COMMON_SRCS}
        ${THREADX_PORT_ASM}
        ${THREADX_PORT_C}        # ← added
        ${ESP32C6_PORT_SRCS}
    ...
)
```

Also renamed our port HAL file from `tx_timer_interrupt.c` to
`tx_esp32c6_timer.c` to avoid any potential confusion with the upstream file
of the same base name.

---

## Bug 26: PLIC EIP_STATUS Bit Already Cleared When We Read It — ISR Silently Skipped Every Tick

**Phase**: ThreadX now starting correctly (bugs 19–25 fixed). Threads run once,
call `tx_thread_sleep()`, then the system freezes. `tx_time_get()` returns 0
forever.

**Symptom**: Both threads print exactly once (blink prints `[blink] tick=0
count=0`, main prints `[main] tick=0 count=0`), then silence. No panic, no
reset, no watchdog. Tick counter stays at 0. System appears completely hung.

**What was actually happening**: The SYSTIMER WAS firing every 10 ms, and the
CPU WAS jumping to vector[7] → `_tx_esp32c6_trap_handler`. But the handler
silently did nothing useful on every single interrupt. After the interrupt, all
threads were still asleep, the scheduler looped with `mstatus.MIE=1`, the next
tick fired 10 ms later, and the cycle repeated. To external observation: a
perfectly silent hang.

**Root cause**: In `_tx_esp32c6_trap_handler`, after calling
`_tx_thread_context_save`, we read the PLIC MX EIP_STATUS register
(`0x2000100C`) and checked whether bit 7 (our CPU interrupt line) was set before
calling `_tx_esp32c6_timer_isr`:

```asm
_handle_interrupt:
    li      t1, PLIC_MX_EIP_STATUS_REG   // 0x2000100C
    lw      t2, 0(t1)                     // read EIP status bitmask
    la      t3, _tx_esp32c6_timer_cpu_int
    lw      t3, 0(t3)                     // cpu_int = 7
    li      t4, 1
    sll     t4, t4, t3                    // t4 = 1 << 7 = 0x80
    and     t5, t2, t4
    beqz    t5, _int_done                 // ← always taken! bit 7 was already 0
    call    _tx_esp32c6_timer_isr         // ← never reached
```

On ESP32-C6, for **edge-triggered** CPU interrupts, the PLIC MX hardware clears
the EIP_STATUS bit the moment the CPU takes the interrupt (i.e., before the
first instruction of the handler executes). By the time our handler read
EIP_STATUS, the bit was already 0 — `beqz t5, _int_done` was always taken —
and `_tx_esp32c6_timer_isr` was **never called**. `_tx_timer_interrupt()` was
never called. The tick counter never incremented. All sleeping threads slept
forever.

**Why this was missed earlier**: The EIP check was meant as a runtime dispatch
to support multiple interrupt sources on the same handler. The reasoning seemed
sound — "if the PLIC says this line is pending, call the ISR". But the edge
clear semantics made the check always false.

**Fix**: Remove the EIP_STATUS check entirely. We use a vectored table, so
vector[7] is called **exclusively** for CPU interrupt line 7 (SYSTIMER). We
already know which interrupt fired — there is no need to re-check at runtime.
Simply call the ISR unconditionally:

```asm
_tx_esp32c6_trap_handler:
    addi    sp, sp, -INT_FRAME_SIZE
    STORE   x1, 28*REGBYTES(sp)
    call    _tx_thread_context_save

    call    _tx_esp32c6_timer_isr   // unconditional — vector[7] = SYSTIMER only

    la      t0, _tx_thread_context_restore
    jr      t0
```

Also removed the now-unused `.equ PLIC_MX_EIP_STATUS_REG` constant and the
`.extern _tx_esp32c6_timer_cpu_int` declaration from the assembly. The C global
`_tx_esp32c6_timer_cpu_int` is kept in `tx_esp32c6_timer.c` for documentation
purposes but is no longer referenced from assembly.

**Key lesson**: In a vectored interrupt architecture, the vector table entry
itself *is* the dispatch. Runtime checks of interrupt-pending registers are
redundant after the CPU has already dispatched, and for edge-triggered
interrupts they are actively harmful because the edge latch is cleared on
interrupt entry.

---

---

## Bug 27: CPU Interrupt Line 7 Is CLINT-Reserved + `mie` Per-Line Bit Not Set

**Phase**: After bug 26 fix. ISR not firing. Diagnostic counter `g_tx_timer_isr_count`
stays 0 despite SYSTIMER being configured.

**Symptom**: Both threads print `tick=0 isr_count=0` on every iteration (busy-wait
loops used instead of `tx_thread_sleep` to rule out sleep issues). Hardware register
readback shows:

```
PLIC ENABLE    = 0x0a000124   ← bit 7 NOT set (despite calling esp_cpu_intr_enable)
PLIC TYPE      = 0x00000000   ← bit 7 NOT set
PLIC PRI[7]    = 0x00000002   ← correctly set to 2
PLIC THRESH    = 0x00000001   ← FreeRTOS default threshold
```

---

### Root Cause Part 1: Line 7 Is Hardware-Reserved (CLINT-Bound)

`esp_cpu_intr_enable(mask)` → `rv_utils_intr_enable(mask)` → `esprv_int_enable(mask)`.
This last step is a ROM function alias for `esprv_intc_int_enable` at address
`0x40000720`, linked via `PROVIDE(esprv_int_enable = esprv_intc_int_enable)` in
`riscv/ld/rom.api.ld`.

The ROM function **silently ignores** reserved lines. From
`esp_hw_support/port/esp32c6/esp_cpu_intr.c`:

```c
// RISC-V Interrupt reserved mask
const uint32_t rsvd_mask = BIT(1) | BIT(3) | BIT(4) | BIT(6) | BIT(7);
/* Interrupts 3, 4 and 7 are unavailable for PULP CPU as they are bound
 * to Core-Local Interrupts (CLINT) */
```

CPU interrupt line 7 is hardwired inside the CPU core to the CLINT (Core-Local
Interrupt Timer, the hardware that drives `mtimecmp`/`mtime`). The PLIC cannot
deliver anything on line 7 — the hardware physically prevents it. Writing the
PLIC ENABLE bit for line 7 has no effect.

Additionally from `intr_alloc.c`:
```
// Muxing an interrupt source to interrupt 6, 7, 11, 15, 16 or 29
// causes the interrupt to effectively be disabled.
```

Line 7 appears in both lists: CLINT-reserved at the CPU level, and effectively
disabled by the INTMTX routing hardware.

**Fix — part 1**: Switch from CPU interrupt line 7 to line **17**. Line 17 is
free for application use (not in any reserved mask). Updated `TIMER_CPU_INT_NUM`
from 7 to 17 in `tx_esp32c6_timer.c` and moved the trap handler entry from
`vector[7]` to `vector[17]` in the vector table.

---

### Root Cause Part 2: The `mie` CSR Per-Line Enable Bit

After switching to line 17 and writing PLIC registers directly (bypassing the
ROM function), a second diagnostic showed:

```
PLIC ENABLE    = 0x0a020124   ← bit 17 SET ✓
PLIC TYPE      = 0x00020000   ← bit 17 SET (edge-triggered) ✓
PLIC PRI[17]   = 0x00000002   ← priority 2 > threshold 1 ✓
PLIC THRESH    = 0x00000001   ← FreeRTOS default ✓
INTMTX src57   = 0x00000011   ← routes to line 17 ✓
mie            = 0x0a000924   ← bit 17 = 0 ✗
mideleg        = 0x00000011   ← bit 17 = 0 (machine mode) ✓
isr_count      = 0            ← ISR still not reached
```

The PLIC is correctly configured but the ISR never fires. Key observation: the
`mie` CSR shows bit 17 = 0.

#### The `mie` CSR Per-Line Requirement (ESP32-C6 Specific)

Standard RISC-V PLIC design requires only two things for external interrupts:
1. `mie.MEIE` (bit 11) — the global "all external interrupts" gate
2. PLIC priority/enable configuration

The ESP32-C6 PLIC is non-standard. It requires **three** things:
1. `mie.MEIE` (bit 11) — global external interrupt gate
2. `PLIC_MXINT_ENABLE_REG bit N` — PLIC enable for CPU line N
3. **`mie CSR bit N`** — per-line CPU enable for line N ← non-standard, not documented

Evidence: Checking FreeRTOS's working interrupt lines (2, 5, 8, 25, 27) with
the same diagnostic code shows their bits are set in **both** `PLIC_ENABLE` and
`mie`. Our line 17 had only `PLIC_ENABLE bit 17` set; `mie bit 17` was 0.

#### Why Direct Writes Miss This

The ROM function `esprv_intc_int_enable` does both internally:
```c
// Pseudocode of the ROM function (from disassembly inference):
PLIC_MXINT_ENABLE_REG |= mask;
csrs mie, mask;          // <-- sets per-line mie bits too
```

Our direct volatile write to `PLIC_MXINT_ENABLE_REG` only did step 1. The
`csrs mie` step was missing. This is why bypassing the ROM API broke things
in a non-obvious way.

**Fix — part 2**: After writing to PLIC_ENABLE, also set the per-line `mie` bit:

```c
/* Enable CPU interrupt line 17 in PLIC ENABLE register */
PLIC_MX_ENABLE |= (1u << TIMER_CPU_INT_NUM);

/* CRITICAL: Also set mie CSR bit 17.
 * ESP32-C6 PLIC requires BOTH PLIC_ENABLE bit N AND mie bit N.
 * The standard mie.MEIE (bit 11) alone is not sufficient.
 * The ROM esprv_intc_int_enable() does "csrs mie, mask" internally;
 * our direct register writes only set PLIC_ENABLE. */
__asm__ volatile("csrs mie, %0" :: "r"(1u << TIMER_CPU_INT_NUM));
```

---

### Additional Fixes Applied in This Investigation

**`mideleg` — keep interrupt in machine mode**:

`mideleg` is a CSR that controls which interrupts are delegated from machine
mode to supervisor mode. If bit N of `mideleg` is 1, interrupt line N is
handled in S-mode (supervisor), bypassing machine-mode handlers entirely.
ThreadX runs in M-mode, so bit 17 must be 0:

```c
__asm__ volatile("csrc mideleg, %0" :: "r"(1u << TIMER_CPU_INT_NUM));
```

`esp_intr_alloc()` does this same write (`RV_CLEAR_CSR(mideleg, BIT(intr))`)
for every interrupt it allocates.

**Priority 2 instead of 1**:

FreeRTOS startup sets `PLIC_MXINT_THRESH_REG = 1` (the constant
`RVHAL_INTR_ENABLE_THRESH`). The PLIC only delivers interrupts with priority
**strictly greater than** the threshold. Priority 1 ≤ threshold 1 → masked.
Changed to priority 2.

**Direct PLIC register writes instead of ROM API**:

We keep the direct volatile writes (rather than reverting to `esp_cpu_intr_*`)
because:
- The ROM function silently drops reserved line masks (line 7 issue)
- We now explicitly control all three requirements (PLIC_ENABLE, mie bit, mideleg)
- The behavior is transparent and auditable from the source code

---

### Summary of All Bug 27 Changes

| What changed | Before | After |
|---|---|---|
| `TIMER_CPU_INT_NUM` | 7 | 17 |
| `TIMER_CPU_INT_PRIORITY` | 1 | 2 |
| Vector table | trap handler at `vector[7]` | trap handler at `vector[17]` |
| PLIC setup | `esp_cpu_intr_*` API | direct volatile register writes |
| `mideleg` | not cleared | `csrc mideleg, (1u<<17)` |
| `mie` bit N | not set (only bit 11 MEIE) | `csrs mie, (1u<<17)` |

---

## Current Status (as of this writing)

Bugs 1–9: confirmed fixed (build succeeds, pre-hardware verification).
Bugs 10–18: HAL-based rewrites applied.
Bugs 19–26: confirmed fixed — ThreadX boots, threads run, ISR dispatch correct.
Bug 27: fix applied — pending hardware verification that `isr_count` increments.

## Remaining Questions for Audit

1. **`mie` per-line bits after context restore**: `_tx_thread_context_restore`
   restores `mstatus` via `mret` (restoring `mstatus.MPIE` → `mstatus.MIE`).
   It does NOT touch the `mie` CSR. Per-line `mie` bits we set in init should
   persist — but verify they are not cleared by any ESP-IDF startup code after
   our init runs.

2. **FreeRTOS threshold**: Verify `PLIC_MXINT_THRESH_REG` value at the point
   `_tx_port_setup_timer_interrupt` runs. FreeRTOS may set it to 1 before or
   after our init — the order matters. Current diagnostic reads it back; confirm
   it shows 1, not a higher value that would block priority 2.

3. **Counter unit selection**: Current code uses SYSTIMER_COUNTER_OS_TICK
   (counter 1) rather than counter 0. Verify counter 1 is not already in use
   by any ESP-IDF component when running without the FreeRTOS scheduler.

4. **Watchdog**: If tick never fires, the ESP-IDF task WDT will eventually
   reset the chip. The diagnostic busy-wait loops prevent this during
   investigation but add noise to timing measurements.

---

## Bug 28 Investigation: isr_count=0 Persists — All Hardware Verified Correct

**Phase**: After Bug 27 fix applied and flashed. Threads run and print; timer
ISR (`g_tx_timer_isr_count`) never increments; tick (`tx_time_get()`) stays 0.

### Exact Serial Output (Hardware Run)

```
I (252) threadx_startup: === ThreadX taking over (port_start_app_hook) ===
I (252) threadx_startup: Entering ThreadX kernel — FreeRTOS scheduler will not start
I (259) tx_diag: --- Timer HW State ---
I (262) tx_diag: mtvec          = 0x4080af01  (should match vector_table addr)
--- 0x4080af01: _tx_esp32c6_vector_table at tx_initialize_low_level.S:74
I (269) tx_diag: mie            = 0x0a020924  (bit11 MEIE must be 1 = 0x800)
I (276) tx_diag: mideleg        = 0x00000011  (bit17 must be 0)
I (282) tx_diag: INTMTX src57   = 0x00000011  (must be 17 = cpu_int)
I (288) tx_diag: PLIC ENABLE    = 0x0a020124  (bit17 must be set = 0x20000)
I (295) tx_diag: PLIC TYPE      = 0x00020000  (bit17 must be set = edge)
I (301) tx_diag: PLIC PRI[7]    = 0x00000002  (must be > THRESH)
I (307) tx_diag: PLIC THRESH    = 0x00000001  (must be < PRI[7])
I (313) tx_diag: ----------------------
I (316) threadx_startup: tx_application_define: setting up system resources
I (323) threadx_startup: ThreadX application defined — main thread created
I (330) threadx_startup: Main thread started, calling app_main()
I (335) main: ============================================
I (340) main:   ThreadX on ESP32-C6 — Demo Application
I (345) main: ============================================
I (351) main: ThreadX version: 6
I (354) main: Tick rate: 100 Hz
I (457) main: [blink] tick=0  isr_count=0  count=0
I (557) main: [blink] tick=0  isr_count=0  count=1
I (657) main: [blink] tick=0  isr_count=0  count=2
I (757) main: [blink] tick=0  isr_count=0  count=3
I (857) main: [blink] tick=0  isr_count=0  count=4
```

Note: `[blink]` prints are tagged `main` in the log because of the esp_log TAG.
Both blink_thread and main_thread run and print at their busy-wait cadence.
Tick and isr_count stay at 0 indefinitely. No crash, no watchdog reset.

---

### Register-by-Register Analysis

| Register | Value | Bit(s) checked | Status |
|---|---|---|---|
| mtvec | `0x4080af01` | top 24 bits = `0x4080af00` = `_tx_esp32c6_vector_table` | ✓ Correct |
| mie | `0x0a020924` | bit 11 (MEIE) = `0x924 & 0x800 = 0x800` ≠ 0 → **set** | ✓ |
| mie | `0x0a020924` | bit 17 = `0x0a020924 & 0x20000 = 0x20000` ≠ 0 → **set** | ✓ |
| mideleg | `0x00000011` | bit 17 = `0x11 & 0x20000 = 0` → **not delegated** | ✓ |
| INTMTX src57 | `0x00000011` | = 17 (decimal) → routes to CPU line 17 | ✓ |
| PLIC ENABLE | `0x0a020124` | bit 17 = `0x0a020124 & 0x20000 = 0x20000` ≠ 0 | ✓ |
| PLIC TYPE | `0x00020000` | bit 17 → edge-triggered | ✓ |
| PLIC PRI[17] | `0x00000002` | priority 2 > threshold 1 | ✓ |
| PLIC THRESH | `0x00000001` | threshold 1 < priority 2 | ✓ |

All checked registers are correctly configured. The interrupt should fire. Yet
`isr_count` stays 0.

**Note on diagnostic label**: The print says `PLIC PRI[7]` but the code reads
`PLIC_MX_PRI_N = PLIC_REG(0x10 + TIMER_CPU_INT_NUM * 4)` = line 17's priority
register. The label is cosmetically wrong; the value is correct.

---

### Two Missing Diagnostic Values

The diagnostic does NOT print two important values:

**1. `mstatus` CSR** — controls whether the CPU actually accepts interrupts
during thread execution. We print `mie` (per-interrupt enables) but not
`mstatus.MIE` (the global gate).

**2. SYSTIMER_CONF_REG (`0x60058000`)** — controls whether counter 1
(SYSTIMER_COUNTER_OS_TICK) is actually running. The HAL calls
`systimer_hal_enable_counter` which should set bit 31 of this register.
If that bit is 0, the counter is stopped and the alarm never fires.

---

### Candidate Root Cause A: `mstatus.MIE = 0` During Thread Execution

The ThreadX scheduler (`tx_thread_schedule.S`, risc-v64/gnu port) hardcodes
`mstatus = 0x1880` before every `mret` when dispatching a thread:

```asm
; From tx_thread_schedule.S lines 203–208 (risc-v64/gnu port)
li    t0, 0x1880    ; MPIE=1 (bit 7), MPP=M-mode (bits 12:11), MIE=0 (bit 3)
csrw  mstatus, t0
mret                ; mret: MIE ← MPIE = 1 → thread runs with MIE = 1
```

The value `0x1880` decoded:
- bit  3 (MIE)  = 0 — cleared now, BEFORE mret
- bit  7 (MPIE) = 1 — "previous interrupt enable" = 1
- bits 12:11 (MPP) = 11 — previous privilege = machine mode

`mret` does: `MIE ← MPIE` (so MIE becomes 1), `MPIE ← 1`, `privilege ← MPP`.
**After mret, threads should run with MIE = 1.**

However, if something modifies `mstatus.MIE` to 0 between mret and the SYSTIMER
alarm firing (e.g., a stray `csrci mstatus, 8` in some ESP-IDF code that runs
during thread startup), interrupts would be blocked.

**To verify**: Read `mstatus` from inside the blink thread entry function:

```c
uint32_t mstatus_val;
__asm__ volatile("csrr %0, mstatus" : "=r"(mstatus_val));
ESP_LOGI(TAG, "[blink] mstatus = 0x%08lx  (bit3 MIE must be 1)", mstatus_val);
```

If `mstatus_val & 0x8 == 0`, MIE is disabled — interrupts are globally masked
during thread execution and the SYSTIMER alarm can never be taken.

---

### Candidate Root Cause B: SYSTIMER Counter 1 Not Running

The HAL sequence calls:
1. `systimer_ll_reset_register()` — resets SYSTIMER_CONF_REG to 0 (clears ALL bits)
2. `systimer_hal_enable_counter(&hal, SYSTIMER_COUNTER_OS_TICK)` — should set
   the counter-1 enable bit (bit 31 of SYSTIMER_CONF_REG = `timer_unit1_work_en`)

If step 2 does not correctly set bit 31, counter 1 stays at value 0 forever.
The alarm (which fires when the counter reaches `current_count + period`) would
see the counter never advancing — the alarm threshold is also 0 but after reset
is immediately written to a non-zero value, so the counter would need to wrap
around the 52-bit space (never in practice).

**To verify**: Read SYSTIMER_CONF_REG from the diagnostic:

```c
uint32_t systimer_conf = *(volatile uint32_t *)0x60058000;
ESP_LOGI("tx_diag", "SYSTIMER_CONF   = 0x%08lx  (bit31 counter1_en must be 1)",
         systimer_conf);
```

Bit 31 = `timer_unit1_work_en`. If 0, counter 1 is stopped.

---

### Secondary Bug Found: `_tx_thread_context_restore.S` Wrong Offset for RV32

While auditing the port assembly, a secondary bug was identified in the upstream
`threadx/ports/risc-v64/gnu/src/tx_thread_context_restore.S`.

**File**: `components/threadx/threadx/ports/risc-v64/gnu/src/tx_thread_context_restore.S`

**Location**: The `_tx_thread_no_preempt_restore` path (non-nested interrupt
return, no thread switch).

**Bug** (approximate line 263):
```asm
; THIS IS WRONG on RV32 (ESP32-C6):
LOAD  t0, 240(sp)   ; hardcoded 240 instead of 30*REGBYTES
csrw  mepc, t0
```

The constant `240` is correct for RV64 (`30 * 8 = 240`) but wrong for RV32
(`30 * 4 = 120`). On ESP32-C6 (RV32), this loads `mepc` from `sp + 240`
instead of `sp + 120`, reading garbage from the wrong stack location.

**Effect**: This bug fires on the **first successful ISR return** that has no
pending thread switch. The `mret` instruction will jump to a garbage address,
causing an immediate instruction-fetch exception. This would present as a crash
immediately after the first SYSTIMER ISR fires — but since `isr_count = 0`
currently, this bug has not yet been triggered.

**Why this is not the current problem**: The ISR never fires at all (isr_count
stays 0), so the context restore path is never reached. This is a secondary bug
that will need to be fixed once the primary ISR-not-firing problem is resolved.

The `LOAD` macro from `tx_port.h` correctly expands to `lw` on RV32 and `ld`
on RV64 — so `LOAD t0, 240(sp)` compiles correctly as `lw t0, 240(sp)` on
RV32. The macro is not the bug; the hardcoded offset `240` is the bug.

**Fix (to apply after primary issue resolved)**:
```asm
; Change from:
LOAD  t0, 240(sp)

; Change to:
LOAD  t0, 30*REGBYTES(sp)   ; = 120 on RV32, = 240 on RV64
```

This is equivalent to `lw t0, 120(sp)` on ESP32-C6. It reads `mepc` from the
correct stack slot regardless of XLEN.

---

### Proposed Diagnostic Additions (Not Yet Applied)

Three additions to narrow down the root cause:

**1. Add `mstatus` readback to `_tx_port_setup_timer_interrupt`** (in
`components/threadx/port/tx_esp32c6_timer.c`):

```c
uint32_t mstatus_val;
__asm__ volatile("csrr %0, mstatus" : "=r"(mstatus_val));
ESP_LOGI("tx_diag", "mstatus        = 0x%08lx  (bit3 MIE, bit7 MPIE)", mstatus_val);
```

**2. Add SYSTIMER_CONF_REG readback to same diagnostic block**:

```c
ESP_LOGI("tx_diag", "SYSTIMER_CONF  = 0x%08lx  (bit31 ctr1_en, bit30 ctr0_en, bit24 alarm0_en)",
         *(volatile uint32_t *)0x60058000);
```

**3. Add `mstatus` print inside `blink_thread_entry` before the loop** (in
`main/main.c`):

```c
static void blink_thread_entry(ULONG param)
{
    uint32_t ms_val;
    __asm__ volatile("csrr %0, mstatus" : "=r"(ms_val));
    ESP_LOGI(TAG, "[blink] mstatus at thread start = 0x%08lx  (bit3 MIE must be 1)", ms_val);
    // rest of loop...
}
```

If Root Cause A: the thread-entry print will show `mstatus & 0x8 == 0`.
If Root Cause B: the diagnostic print will show `SYSTIMER_CONF bit31 == 0`.

---

### Current Status After Bug 28 Analysis

- All PLIC / INTMTX / mie / mideleg / mtvec registers: **verified correct**
- `isr_count` = 0: **ISR never reached** — root cause not yet pinned
- Two candidates identified: mstatus.MIE = 0 (A) or SYSTIMER counter stopped (B)
- Secondary bug in `_tx_thread_context_restore.S` at `LOAD t0, 240(sp)`:
  **identified, not yet fixed** (harmless until ISR fires)
- Next action: add `mstatus` + SYSTIMER_CONF_REG diagnostics, rebuild, flash

---

## Bug 28 (continued) — Second Diagnostic Run and Root Cause Identified

### Second Diagnostic Run Output (corrected SYSTIMER offsets)

```
I (259) tx_diag: --- Timer HW State ---
I (263) tx_diag: mtvec          = 0x4080af01  (should match vector_table addr)
--- 0x4080af01: _tx_esp32c6_vector_table at tx_initialize_low_level.S:74
I (270) tx_diag: mstatus        = 0x00000009  (bit3 MIE, bit7 MPIE, bits12:11 MPP)
I (277) tx_diag: mie            = 0x0a020924  (bit11 MEIE must be 1 = 0x800)
I (284) tx_diag: mideleg        = 0x00000011  (bit17 must be 0)
I (289) tx_diag: INTMTX src57   = 0x00000011  (must be 17 = cpu_int)
I (295) tx_diag: PLIC ENABLE    = 0x0a020124  (bit17 must be set = 0x20000)
I (302) tx_diag: PLIC TYPE      = 0x00020000  (bit17 must be set = edge)
I (308) tx_diag: PLIC PRI[17]   = 0x00000002  (must be > THRESH)
I (314) tx_diag: PLIC THRESH    = 0x00000001  (must be < PRI[17])
I (320) tx_diag: SYSTIMER base  = 0x6000a000  (expect 0x6000A000)
I (326) tx_diag: SYSTIMER_CONF  = 0xf7000002  (bit31 clk_en, bit29 ctr1_en, bit24 alm0_en)
I (334) tx_diag: SYSTIMER TGT0  = 0xc0027100  (bit31=ctr_sel, bit30=period, [25:0]=ticks)
I (342) tx_diag: SYSTIMER INTENA= 0x00000005  (bit0=alm0 int enabled)
I (348) tx_diag: SYSTIMER INT_ST= 0x00000001  (bit0=alm0 currently pending)
I (354) tx_diag: ----------------------
I (358) threadx_startup: tx_application_define: setting up system resources
I (365) threadx_startup: ThreadX application defined — main thread created
I (372) threadx_startup: Main thread started, calling app_main()
I (377) main: ============================================
I (382) main:   ThreadX on ESP32-C6 — Demo Application
I (393) main: ThreadX version: 6
I (396) main: Tick rate: 100 Hz
I (398) main: [blink] mstatus at thread start = 0x00000088  (bit3 MIE must be 1)
I (506) main: [blink] tick=0  isr_count=0  count=0
... (isr_count stays 0 indefinitely)
```

### Full Register Analysis

| Register | Value | Interpretation |
|----------|-------|----------------|
| mtvec | 0x4080af01 | ✓ Our vector table, vectored mode (bit0=1) |
| mstatus | 0x00000009 | ✓ MIE=1 (bit3), UIE=1 (bit0) |
| mie | 0x0a020924 | ✓ MEIE=1 (bit11), line17=1 (bit17), FreeRTOS lines (2,5,8,25,27) |
| mideleg | 0x00000011 | ✓ bit17=0 (not delegated), bits0,4 are U-mode timer/SW |
| INTMTX src57 | 0x00000011=17 | ✓ Source 57 (SYSTIMER_TARGET0) → CPU line 17 |
| PLIC ENABLE | 0x0a020124 | ✓ bit17 set |
| PLIC TYPE | 0x00020000 | **bit17=1 = EDGE-triggered** ← ROOT CAUSE |
| PLIC PRI[17] | 0x00000002 | ✓ Priority 2 > threshold 1 |
| PLIC THRESH | 0x00000001 | ✓ Threshold 1 |
| SYSTIMER CONF | 0xf7000002 | ✓ Counter1 running (bit29), alarm0 enabled (bit24) |
| SYSTIMER TGT0 | 0xc0027100 | ✓ Counter1 selected (bit31), periodic (bit30), 160000 ticks |
| SYSTIMER INTENA | 0x00000005 | ✓ bit0=alarm0 int enabled |
| SYSTIMER INT_ST | 0x00000001 | Alarm 0 pending — interrupt signal asserted |
| mstatus in thread | 0x00000088 | ✓ MIE=1 (bit3), MPIE=1 (bit7) |

### Root Cause: Edge-Triggered Mode

**All hardware registers are correctly configured EXCEPT the PLIC trigger mode.**

We set `PLIC_MX_TYPE |= (1u << 17)` which configures line 17 as **edge-triggered**.
This was wrong because:

1. Setup sequence: configure SYSTIMER → start counter → configure PLIC (TYPE/ENABLE/mie)
2. SYSTIMER alarm fires ~10ms after counter starts
3. By the time we reach the diagnostic prints (~90ms elapsed), INT_ST=1 (alarm has fired)
4. With edge-triggered mode: the PLIC detects a **0→1 rising edge** on the input to set its latch
5. **Problem**: if the SYSTIMER output was already HIGH (INT_ST=1) when the PLIC TYPE bit
   was written (step 3a in setup), the PLIC never saw a rising edge to latch
6. More precisely: even if we set TYPE=edge and ENABLE before the first alarm fires,
   the setup code clears the edge latch (`PLIC_MX_CLEAR`) immediately after enabling.
   Then INT_ST stays HIGH permanently (ISR never clears it). In periodic mode without
   the ISR running, INT_ST stays asserted. With edge-triggered PLIC, there is no
   NEW rising edge → PLIC latch never gets set → mip.bit17 never asserted → ISR never fires.

**The key insight**: FreeRTOS (`port_systick.c vSystimerSetup`) uses `esp_intr_alloc()`
without `ESP_INTR_FLAG_EDGE` → **level-triggered by default**. Level-triggered means:
- While SYSTIMER INT_ST=1, PLIC continuously asserts mip.bit17
- CPU takes interrupt → ISR clears INT_ST → output goes LOW → PLIC deasserts automatically
- No PLIC CLEAR register write needed in ISR

### Fix Applied (Bug 28)

1. **`tx_esp32c6_timer.c` setup**: Changed from `PLIC_MX_TYPE |= (1u << 17)` to
   `PLIC_MX_TYPE &= ~(1u << 17)` (level-triggered)
2. **`tx_esp32c6_timer.c` setup**: Removed `PLIC_MX_CLEAR |= (1u << 17)` (not needed for level)
3. **`_tx_esp32c6_timer_isr`**: Removed `PLIC_MX_CLEAR |= (1u << N)` from ISR
4. **Diagnostic**: Added `mip` CSR readout and `PLIC_EMIP` (edge pending status) readout
5. **Diagnostic**: Fixed `PLIC TYPE` label from "must be set = edge" to "must be 0 = level"

With level-triggered operation the ISR just needs to call `systimer_ll_clear_alarm_int()`
which drops the SYSTIMER output LOW, causing the PLIC to automatically deassert mip.bit17.

---
## Bug 29: Complete System Hang After Level-Triggered Fix — ISR Fires Before Threads Exist

**Phase**: After Bug 28 fix (level-triggered PLIC)

**Symptom**: After switching to level-triggered PLIC mode (Bug 28 fix), the system
produces **no output at all** from any thread. The last message printed is from
the startup hook:

```
I (252) threadx_startup: === ThreadX taking over (port_start_app_hook) ===
I (252) threadx_startup: Entering ThreadX kernel — FreeRTOS scheduler will not start
```

Then complete silence — `app_main()` never prints, `blink_thread_entry` never prints,
tick counter never increments. The device appears to hang indefinitely.

**What changed**: Bug 28 fix switched PLIC from edge-triggered to level-triggered.
This means the PLIC now continuously asserts mip.bit17 as long as SYSTIMER INT_ST=1.

**Root Cause: Timer ISR fires before tx_application_define() creates any threads**

Execution sequence that causes the hang:

```
port_start_app_hook()
  → tx_kernel_enter()
    → _tx_initialize_low_level()           ← sets up timer, csrs mie, 0x800
      → _tx_port_setup_timer_interrupt()   ← arms SYSTIMER alarm (10ms to first tick)
      ← returns (mstatus.MIE still 1 from ESP-IDF!)
    ← returns
    → tx_application_define()             ← would create threads, but...
```

**Problem**: ESP-IDF startup leaves `mstatus.MIE=1` (global interrupt enable).
After `_tx_port_setup_timer_interrupt()` arms the SYSTIMER with level-triggered PLIC:

1. SYSTIMER counts down, alarm fires in ~10ms
2. SYSTIMER INT_ST goes HIGH
3. PLIC sees HIGH input, asserts mip.bit17 continuously
4. CPU takes interrupt (mstatus.MIE=1 allows it)
5. CPU jumps to vector[17] → `_tx_esp32c6_trap_handler`
6. `call _tx_thread_context_save` — saves context of... what? No threads exist yet
7. `call _tx_esp32c6_timer_isr` — clears INT_ST, calls `_tx_timer_interrupt()`
8. `jr _tx_thread_context_restore` — tries to restore next thread to run
9. **`_tx_thread_execute_ptr == NULL`** (no threads created yet!)
10. `_tx_thread_context_restore` enters its idle-wait path
11. **HANGS FOREVER in idle-wait** — `tx_application_define()` never executes,
    no threads ever created, no output ever appears

The key: with level-triggered mode, the timer ISR fires "too early" — before the
kernel has a chance to run `tx_application_define()` and create any threads.
With edge-triggered mode (Bug 28's wrong setting), the ISR never fired at all
(isr_count=0). With level-triggered + `mstatus.MIE=1`, the ISR fires at the
worst possible moment.

**Why edge-triggered didn't show this bug**: Because with edge-triggered mode
the ISR never fired at all — so the system degraded differently (threads ran
but `tx_time_get()` never incremented). Level-triggered works correctly once
the threading infrastructure is in place, but fires too early.

**Fix: Disable mstatus.MIE at the start of _tx_initialize_low_level**

Standard ThreadX practice: the low-level init function disables global interrupts
at entry. The scheduler's first `mret` (in `_tx_thread_schedule`) restores
`mstatus.MPIE → MIE = 1`, re-enabling interrupts at exactly the right moment
(when the first thread is about to run).

Added at the very beginning of `_tx_initialize_low_level` in `tx_initialize_low_level.S`:

```asm
_tx_initialize_low_level:
    /* 0. Disable global machine interrupts for the duration of initialization.
     *
     *    mstatus.MIE (bit 3) may be 1 from ESP-IDF startup. We MUST clear it
     *    before configuring the timer, because:
     *
     *    With level-triggered PLIC (correct for SYSTIMER), as soon as the first
     *    SYSTIMER alarm fires (~10ms after setup), the PLIC asserts mip.bit17.
     *    If mstatus.MIE=1 at that moment, the CPU takes the interrupt, enters
     *    _tx_esp32c6_trap_handler → _tx_thread_context_save, calls _tx_timer_interrupt(),
     *    then calls _tx_thread_context_restore. If tx_application_define() has not
     *    run yet, _tx_thread_execute_ptr=NULL, and _tx_thread_context_restore
     *    enters its idle-wait path and HANGS FOREVER — tx_application_define()
     *    never gets a chance to create any threads.
     *
     *    Fix: clear MIE here. The scheduler's first mret (in _tx_thread_schedule)
     *    restores mstatus.MPIE→MIE=1, re-enabling interrupts at exactly the right
     *    moment. This is standard ThreadX practice.
     */
    csrci   mstatus, 0x8        /* clear MIE (bit 3) — disable global interrupts */
```

**Why this works**:

```
_tx_initialize_low_level:
  csrci mstatus, 0x8           ← MIE now 0 — timer ISR cannot fire
  ... set up timer, mie.MEIE ...
  ret                          ← still MIE=0, mip.bit17 pending but not taken

tx_application_define():
  tx_thread_create(app_thread) ← threads created safely, no ISR interference
  return

_tx_thread_schedule:
  ... find first runnable thread ...
  mret                         ← MPIE=1 → MIE=1, first thread runs
                               ← NOW timer ISR fires (threads exist, all is well)
```

**The mie.MEIE interaction**: Step 4 in `_tx_initialize_low_level` sets `mie.MEIE`
(bit 11, which gates PLIC delivery). This is separate from `mstatus.MIE` (bit 3,
global gate). Setting `mie.MEIE` while `mstatus.MIE=0` is safe — the pending
interrupt will be remembered in `mip.bit17` and delivered when `mstatus.MIE=1`
is restored by `mret` in `_tx_thread_schedule`.

**Lesson**: When integrating a new RTOS into a system that leaves MIE=1 during
startup (like ESP-IDF), the RTOS's low-level init MUST disable global interrupts
explicitly. Do not rely on the caller to have left them disabled.

---
## System Working — Final Verified State (After Bug 29 Fix)

**Status**: ThreadX fully operational on ESP32-C6 as of this milestone.

**Verified serial output**:
```
I (252) threadx_startup: === ThreadX taking over (port_start_app_hook) ===
I (252) threadx_startup: Entering ThreadX kernel — FreeRTOS scheduler will not start
...
I (374) threadx_startup: tx_application_define: setting up system resources
I (381) threadx_startup: ThreadX application defined — main thread created
I (387) threadx_startup: Main thread started, calling app_main()
...
I (417) main: [main]  tick=4  isr_count=4  count=0
I (422) main: [blink] mstatus at thread start = 0x00000088  (bit3 MIE must be 1)
I (429) main: [blink] tick=5  isr_count=5  count=0
I (529) main: [blink] tick=16  isr_count=16  count=1
I (619) main: [main]  tick=25  isr_count=25  count=1
I (629) main: [blink] tick=26  isr_count=26  count=2
I (729) main: [blink] tick=36  isr_count=36  count=3
I (819) main: [main]  tick=45  isr_count=45  count=2
```

**Confirmed working**:
- ThreadX scheduler boots and creates threads correctly
- SYSTIMER tick fires at exactly 100 Hz (`tick == isr_count` in every line)
- Two threads interleave: `[main]` (priority 16) and `[blink]` (priority 5)
- `mstatus=0x00000088` in blink thread: bit 3 (MIE=1) and bit 7 (MPIE=1) — interrupts
  enabled inside thread context as expected
- `tx_thread_sleep()` works correctly — threads block for the specified number of ticks
  and other threads run during the sleep period

**Complete bug chain resolved**: Bugs 19 → 20 → 21 → 22–27 → 28 → 29 all fixed.

---

## Note: tx_thread_relinquish vs tx_thread_sleep — Scheduling Behavior

**Observation during testing**: Using `tx_thread_relinquish()` in both threads
did NOT produce interleaving output when the two threads had different priorities.
Only after switching to `tx_thread_sleep()` did both threads print alternately.

**Explanation**:

`tx_thread_relinquish()` yields only to threads of the **same priority**. In ThreadX,
lower priority number = higher priority. The two threads have:
- `blink_thread`: priority 5 (`BLINK_THREAD_PRIO = 5`)
- `main_thread`: priority 16 (`MAIN_THREAD_PRIORITY = 16`)

When blink (priority 5) calls `tx_thread_relinquish()`, ThreadX looks for another
READY thread at priority 5. There is none, so blink immediately resumes. Main
(priority 16) never gets CPU time because blink (priority 5) is always ready and
preempts it.

`tx_thread_sleep(N)` **suspends** (blocks) the calling thread for N ticks. A suspended
thread is removed from the READY queue entirely, so the scheduler can pick any other
runnable thread regardless of priority. This is why sleep produces correct interleaving.

**Summary**:

| API | Behavior | Cross-priority yielding? |
|-----|----------|--------------------------|
| `tx_thread_relinquish()` | Yields to same-priority ready threads only | No |
| `tx_thread_sleep(N)` | Blocks caller for N ticks, any thread can run | Yes |
| `tx_thread_suspend(t)` | Blocks target thread indefinitely | Yes |

For cooperative multitasking between threads of **different priorities**, always use
`tx_thread_sleep()` or a blocking object (semaphore, mutex, event flags, queue).
`tx_thread_relinquish()` is only useful for round-robin fairness within the same
priority level.

---

## Menuconfig RTOS Selection + Kconfig Parameter Wiring

**Phase**: Post-working-system housekeeping. ThreadX fully operational (Bugs 1–29
resolved). Goal: make the RTOS choice user-selectable via `idf.py menuconfig` and
wire the four Kconfig parameters to actual source code instead of hardcoded values.

### Changes Implemented

#### 1. New Kconfig: RTOS Selection Choice

`components/threadx/Kconfig` was completely replaced. Previously contained only a
`menu "ThreadX Configuration"` with four hardcoded parameters (one with a wrong
default). Replaced with:

- **`menu "RTOS Selection"` with `depends on IDF_TARGET_ESP32C6`**: The entire
  menu only appears when targeting ESP32-C6, preventing accidental selection on
  unsupported targets.

- **`choice RTOS_SELECTION`**: Two options — `RTOS_SELECTION_FREERTOS` (standard
  ESP-IDF FreeRTOS) and `RTOS_SELECTION_THREADX` (Azure RTOS, ESP32-C6 only).
  Default: `RTOS_SELECTION_THREADX` (preserves current project behavior; also
  used as the kconfig fallback when an existing sdkconfig lacks this new choice,
  see Bug 30 below).

- **`menu "ThreadX Configuration"` with `depends on RTOS_SELECTION_THREADX`**: The
  three wirable parameters only appear when ThreadX is selected.

- **`THREADX_TIMER_CPU_INT` removed entirely**: This parameter had the wrong default
  (7 instead of 17) and cannot be wired to the assembly vector table — the `j`
  instruction at `vector[17]` in `tx_initialize_low_level.S` is a fixed assembler
  offset. Making it configurable would require fragile `.rept` tricks. CPU line 17
  is the correct fixed choice by hardware analysis (Bug 27). The parameter was a
  misleading dead config.

- **Ranges added**: `THREADX_TICK_RATE_HZ` 10–1000, `THREADX_MAX_PRIORITIES` 8–256,
  `THREADX_BYTE_POOL_SIZE` 4096–131072. Prevents nonsensical values from compiling.

#### 2. tx_user.h — Wire Tick Rate and Max Priorities

`components/threadx/include/tx_user.h` changed two hardcoded constants to
`CONFIG_*` macros:

```c
/* Before: */
#define TX_TIMER_TICKS_PER_SECOND       100
#define TX_MAX_PRIORITIES               32

/* After: */
#define TX_TIMER_TICKS_PER_SECOND       CONFIG_THREADX_TICK_RATE_HZ
#define TX_MAX_PRIORITIES               CONFIG_THREADX_MAX_PRIORITIES
```

ESP-IDF automatically adds `-include sdkconfig.h` to every compiled translation
unit, so `CONFIG_THREADX_TICK_RATE_HZ` is available in tx_user.h without an
explicit `#include "sdkconfig.h"`.

The unused `TX_BYTE_POOL_SIZE_DEFAULT (32*1024)` macro was also removed — it was
never referenced anywhere in the codebase and predated the Kconfig parameter.

#### 3. tx_port_startup.c — Wire Byte Pool Size

`components/threadx/port/tx_port_startup.c`:

```c
/* Before: */
#define SYSTEM_BYTE_POOL_SIZE   (32 * 1024)

/* After: */
#define SYSTEM_BYTE_POOL_SIZE   CONFIG_THREADX_BYTE_POOL_SIZE
```

#### 4. CMakeLists.txt Early-Return Guards

`components/threadx/CMakeLists.txt` and `components/freertos_compat/CMakeLists.txt`
each received an early-return guard at the top:

```cmake
if(NOT CONFIG_RTOS_SELECTION_THREADX)
    idf_component_register()
    return()
endif()
```

When FreeRTOS mode is selected, both components register as empty interface targets
and return immediately. Zero ThreadX sources compile. The rest of each CMakeLists.txt
(source lists, `idf_component_register` with full SRCS, `target_*` calls) executes
only in ThreadX mode.

#### 5. main/CMakeLists.txt — Conditional REQUIRES

```cmake
if(CONFIG_RTOS_SELECTION_THREADX)
    idf_component_register(SRCS "main.c" INCLUDE_DIRS "." REQUIRES threadx freertos_compat)
else()
    idf_component_register(SRCS "main.c" INCLUDE_DIRS ".")
endif()
```

In FreeRTOS mode, `main` does not depend on `threadx` or `freertos_compat`
(both empty). This avoids spurious include-path requests to empty components.

#### 6. main/main.c — Dual-Mode Demo

The entire file is wrapped in `#ifdef CONFIG_RTOS_SELECTION_THREADX` / `#else` /
`#endif`. The `#else` branch contains a minimal FreeRTOS demo (two tasks using
`xTaskCreate`, `vTaskDelay`, `xTaskGetTickCount`) so the project compiles and runs
in either mode without modification.

The `extern volatile uint32_t g_tx_timer_isr_count` declaration moved inside the
`#ifdef` block — it references a symbol in `tx_esp32c6_timer.c` which is not linked
in FreeRTOS mode; leaving it outside would cause a link error.

#### 7. sdkconfig.defaults — Preserve Current Behavior

`CONFIG_RTOS_SELECTION_THREADX=y` added to `sdkconfig.defaults`. This ensures fresh
sdkconfig creation (e.g. after `idf.py fullclean`) selects ThreadX without requiring
`idf.py menuconfig`.

---

## Bug 30: component_requirements.py BUG — Nested `threadx/threadx/` Directory Breaks REQUIRES Include Propagation

**Phase**: Immediately after the menuconfig/Kconfig changes above were applied.

**Symptom**:

```
/home/kty/work/threadx-esp32c6-project/components/freertos_compat/src/port.c:11:10:
fatal error: tx_api.h: No such file or directory
   11 | #include <tx_api.h>
      |          ^~~~~~~~~~
compilation terminated.
[1191/1204] Building C object esp-idf/threadx/CMakeFiles/__idf_threadx.dir/port/tx_esp32c6_timer.c.obj
ninja: build stopped: subcommand failed.
BUG: component_requirements.py: cannot match original component filename for source component threadx
ninja failed with exit code 1
```

Two facts stand out:
1. `tx_esp32c6_timer.c` IS being compiled (ThreadX mode is active, early return did NOT fire)
2. Yet `tx_api.h` is not found in `freertos_compat/src/port.c`

This means: `CONFIG_RTOS_SELECTION_THREADX=y`, the threadx component is building
fully, but the `REQUIRES threadx` in `freertos_compat/CMakeLists.txt` is NOT
propagating threadx's public `INCLUDE_DIRS` to freertos_compat's compile command.

**Root Cause: Pre-Existing ESP-IDF Bug Triggered by Kconfig Change**

The ESP-IDF build system calls `component_requirements.py` (a Python script in
`${IDF_PATH}/tools/`) during CMake configuration to process the component
dependency graph and set up include-path propagation. This script had a pre-existing
bug with our component structure.

The `threadx` component directory is `components/threadx/`. Inside it, the git
submodule is at `components/threadx/threadx/` — the **same name as the component
itself**. When `component_requirements.py` tries to match the component named
"threadx" to its source files/directory, the nested `threadx/threadx/` path is
ambiguous. The script throws a Python exception:

```
cannot match original component filename for source component threadx
```

The exception is caught by idf.py and printed as a "BUG:" message. However, the
failed match means the dependency graph entry for `threadx` is incomplete. The
include-directory propagation from `threadx` to `freertos_compat` (via
`REQUIRES threadx`) is never established. freertos_compat's compile command has
no `-I .../threadx/common/inc` flag, so `tx_api.h` is not found.

**Why was this not triggered before?**

The script is called during CMake configuration, which only fully re-runs when
CMakeLists.txt or Kconfig files change. The pre-existing codebase had stable
Kconfig content. Our change (adding the `RTOS_SELECTION` choice to `Kconfig`)
triggered a CMake re-run for the first time since the `threadx/threadx/`
submodule structure was established. This was the first time `component_requirements.py`
was called with this component layout.

The `BUG:` message appears AFTER "ninja failed" in the output because idf.py
also runs the script as a post-processing step when reporting errors — but the
damage is done during the CMake configuration phase that precedes ninja.

**Fix: Explicit `target_include_directories` in freertos_compat**

Added to `components/freertos_compat/CMakeLists.txt`, after `idf_component_register`:

```cmake
# Workaround: explicitly re-export threadx's public include dirs.
# Normally REQUIRES threadx propagates them, but component_requirements.py has
# a pre-existing bug when the component name matches its own submodule directory
# (components/threadx/threadx/).  A Kconfig change triggers the CMake re-run
# that first calls the script, breaking transitive include propagation.
# Adding the paths explicitly here makes REQUIRES irrelevant for this purpose.
idf_component_get_property(threadx_dir threadx COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PUBLIC
    "${threadx_dir}/threadx/common/inc"
    "${threadx_dir}/threadx/ports/risc-v64/gnu/inc"
    "${threadx_dir}/include"
)
```

`idf_component_get_property(threadx_dir threadx COMPONENT_DIR)` returns the
threadx component's source directory (`components/threadx/`). The three paths
appended are identical to those registered in threadx's `idf_component_register`
`INCLUDE_DIRS`. By adding them explicitly via `target_include_directories`, the
include propagation bypasses `component_requirements.py` entirely and works
correctly regardless of whether the script succeeds.

**Additional fix: Kconfig default changed from FREERTOS to THREADX**

A secondary symptom of the same Kconfig change: on projects with an existing
`sdkconfig` (which predates the new `RTOS_SELECTION` choice), ESP-IDF assigns
new Kconfig options using their declared `default`. If that default was
`RTOS_SELECTION_FREERTOS`, the existing sdkconfig would be updated to select
FreeRTOS mode. The early-return guards would then fire for both components,
registering them as empty — but stale ninja build rules from the previous
ThreadX build would still reference `freertos_compat/src/port.c`, leading to
the same `tx_api.h` not found error (now without even an obvious explanation).

Changing the Kconfig default from `RTOS_SELECTION_FREERTOS` to
`RTOS_SELECTION_THREADX` ensures that existing sdkconfigs encountering the new
choice for the first time default to ThreadX, preserving the project's intended
behavior.

**After-action: full clean required**

After any Kconfig structure change (adding/removing/renaming choices), a full
clean build is recommended to eliminate stale CMake cache and ninja rule
artifacts:

```bash
idf.py fullclean && idf.py build
```

`idf.py fullclean` deletes the entire `build/` directory, forcing a fresh CMake
configuration pass and complete recompilation. This eliminates stale ninja rules
that reference sources no longer part of a component's build.

---

## Bug 31: `TX_THREAD` Has No Member `txfr_thread_ptr` — Compile Definition Propagation Also Broken

**Phase**: Immediately after Bug 30 fix (explicit include dirs added). The
`tx_api.h` include error is gone; compilation reaches `tx_freertos.c` and
immediately fails with a new error.

**Symptom** (10 instances of the same error across tx_freertos.c):

```
tx_freertos.c:228:29: error: 'TX_THREAD' {aka 'struct TX_THREAD_STRUCT'} has no member
named 'txfr_thread_ptr'; did you mean 'tx_thread_id'?
  228 |     p_txfr_task = p_thread->txfr_thread_ptr;
```

**Root Cause: Same component_requirements.py BUG, second symptom**

Bug 30 documented that `component_requirements.py` failing breaks transitive
propagation from `threadx` to `freertos_compat` via `REQUIRES`. The Bug 30 fix
added explicit `target_include_directories` for the include paths, which resolved
the `tx_api.h` not found error. But the same broken propagation also affects
**compile definitions**.

The `threadx/CMakeLists.txt` defines:

```cmake
target_compile_definitions(${COMPONENT_LIB} PUBLIC TX_INCLUDE_USER_DEFINE_FILE)
```

This is declared `PUBLIC`, meaning it should propagate to any component with
`REQUIRES threadx`. When the dependency graph is complete, freertos_compat
would inherit `-DTX_INCLUDE_USER_DEFINE_FILE` on its compile command.

When `component_requirements.py` fails, the dependency graph entry for threadx
is incomplete. The `TX_INCLUDE_USER_DEFINE_FILE` flag never reaches freertos_compat.

**The effect of missing TX_INCLUDE_USER_DEFINE_FILE**:

In ThreadX's `tx_api.h`:

```c
/* Determine if the user extension file should be used. */
#ifdef TX_INCLUDE_USER_DEFINE_FILE
#include "tx_user.h"    /* ← only included if the flag is defined */
#endif
```

Without `-DTX_INCLUDE_USER_DEFINE_FILE`, `tx_user.h` is never included.
Without `tx_user.h`, `TX_THREAD_USER_EXTENSION` is never defined.
Without `TX_THREAD_USER_EXTENSION`, the `TX_THREAD_STRUCT` definition (in `tx_thread.h`)
never gains the extra field:

```c
/* From tx_thread.h — the struct definition: */
struct TX_THREAD_STRUCT {
    ...
#ifdef TX_THREAD_USER_EXTENSION
    TX_THREAD_USER_EXTENSION         /* expands to: VOID *txfr_thread_ptr; */
#endif
    ...
};
```

So `TX_THREAD` is compiled without `txfr_thread_ptr`. Every access to that field
in `tx_freertos.c` fails with "has no member named 'txfr_thread_ptr'".

**This is Bug 8 re-appearing**: The first time `txfr_thread_ptr` was missing (Bug 8,
early development), the fix was adding `TX_THREAD_USER_EXTENSION VOID *txfr_thread_ptr;`
to `tx_user.h`. The definition is still there and correct. But now the mechanism
that causes `tx_user.h` to be included (`TX_INCLUDE_USER_DEFINE_FILE`) is not
reaching `tx_freertos.c` due to the broken dependency propagation.

**Fix**: Add explicit `target_compile_definitions` in `freertos_compat/CMakeLists.txt`,
alongside the existing explicit `target_include_directories` workaround:

```cmake
# Without this, tx_api.h never #includes tx_user.h, TX_THREAD_USER_EXTENSION
# is never defined, and txfr_thread_ptr is absent from TX_THREAD.
target_compile_definitions(${COMPONENT_LIB} PUBLIC TX_INCLUDE_USER_DEFINE_FILE)
```

This mirrors what threadx's CMakeLists.txt already does for its own sources —
we're just replicating the PUBLIC definition in freertos_compat since the automatic
propagation path is broken.

**Pattern established**: The `component_requirements.py` bug breaks ALL transitive
propagation from threadx to freertos_compat. Any PUBLIC property on the threadx
component target (include dirs, compile definitions, link options) must be
explicitly re-declared in freertos_compat. To date, two properties are affected:

| Property | threadx declares | freertos_compat workaround |
|---|---|---|
| Include dirs (`common/inc`, `ports/risc-v64/gnu/inc`, `include/`) | `INCLUDE_DIRS` in `idf_component_register` | `target_include_directories` via `idf_component_get_property` |
| Compile definition `TX_INCLUDE_USER_DEFINE_FILE` | `target_compile_definitions PUBLIC` | `target_compile_definitions PUBLIC` |

---

## Bug 35 — `portYIELD_FROM_ISR` Calls Thread-Level API from ISR Context

**Iteration**: 30
**Date**: 2026-02-27

### Symptom

System boots, WiFi STA initializes, both main thread and scanner thread print once
(at tick 32 and 33 respectively), then the system freezes permanently. No more tick
output, no more thread activity.

### Investigation

The freeze occurs ~330ms after boot — exactly when the WiFi driver fires its first
interrupt. ThreadX scheduling works fine before that point (two threads interleave
correctly). The freeze timing correlates with WiFi radio initialization completing.

When the WiFi ISR fires on a non-17 CPU line, it goes through the ESP-IDF interrupt
path (`_interrupt_handler` → `rtos_int_enter` → WiFi handler → `rtos_int_exit`).
Inside the WiFi handler, ESP-IDF calls `portYIELD_FROM_ISR()` to request a context
switch after signaling a semaphore or event group.

### Root Cause

`portYIELD_FROM_ISR(...)` was defined as `vPortYield()` in both `FreeRTOSConfig.h`
and `portmacro.h`. `vPortYield()` calls `tx_thread_relinquish()`, which is a
**thread-level** ThreadX API. Calling it from ISR context is illegal because:

1. It modifies `_tx_thread_current_ptr` (the scheduler's "currently running thread"
   pointer) while the ISR context save/restore expects it to remain stable
2. It manipulates the ready list without ISR-safe protection
3. It attempts a context switch while running on the ISR stack, not a thread stack
4. The subsequent `rtos_int_exit` tries to restore SP from `pxCurrentTCBs`, which
   may now be inconsistent due to the scheduler corruption

**Call chain in the bug:**
```
WiFi ISR (on ISR stack, _tx_thread_system_state > 0)
  → portYIELD_FROM_ISR(pdTRUE)
    → vPortYield()
      → tx_thread_relinquish()    ← ILLEGAL: thread-level API from ISR!
        → modifies _tx_thread_current_ptr
        → corrupts scheduler state
        → system hangs on next context switch
```

**How real FreeRTOS does it correctly:**
```
WiFi ISR (on ISR stack)
  → portYIELD_FROM_ISR(pdTRUE)
    → vPortYieldFromISR()
      → xPortSwitchFlag = 1       ← Just sets a flag! No scheduler call.
  ... ISR continues normally ...
  → rtos_int_exit
    → checks xPortSwitchFlag
    → if set: calls vTaskSwitchContext() to pick highest-priority task
    → restores SP from new task's TCB
    → mret to new task
```

The critical difference: real FreeRTOS defers the actual context switch to the ISR
exit path. Our ThreadX implementation was calling the scheduler directly from within
the ISR handler.

### Fix

Changed `portYIELD_FROM_ISR` and `portEND_SWITCHING_ISR` to no-ops in both files:

**`FreeRTOSConfig.h`:**
```c
#define portYIELD_FROM_ISR(...)             ((void)0)
#define portEND_SWITCHING_ISR(...)          ((void)0)
```

**`portmacro.h`:**
```c
#undef portYIELD_FROM_ISR
#undef portEND_SWITCHING_ISR
#define portYIELD_FROM_ISR(...)         ((void)0)
#define portEND_SWITCHING_ISR(...)      ((void)0)
```

The `#undef` in `portmacro.h` is necessary because `FreeRTOSConfig.h` is included
first (by the upstream compat `FreeRTOS.h`), so `portmacro.h` must explicitly
undefine before redefining to avoid `-Werror=redefine` compilation failure.

### Why No-Op Is Correct

ThreadX handles preemption automatically. When a WiFi ISR signals a ThreadX
semaphore (via the compat layer's `xSemaphoreGive` → `tx_semaphore_put`), ThreadX
internally updates `_tx_thread_execute_ptr` to point to the highest-priority ready
thread. At the next timer tick (≤10ms at 100 Hz), `_tx_thread_context_restore`
compares `_tx_thread_execute_ptr` vs `_tx_thread_current_ptr` and performs the
context switch if they differ.

Trade-off: up to 10ms latency for ISR-to-thread wakeup. Acceptable for WiFi/BLE
events, scan results, connection state changes.

### Future Optimization

The infrastructure for immediate ISR-triggered context switches already exists:
- `rtos_int_hooks.S` has `xPortSwitchFlag` check and `vTaskSwitchContext` call
- `vTaskSwitchContext` is currently a no-op stub in `port.c`

To enable immediate switching:
1. Change `portYIELD_FROM_ISR` to set `xPortSwitchFlag = 1`
2. Implement `vTaskSwitchContext` to update `pxCurrentTCBs` from ThreadX's
   `_tx_thread_execute_ptr`

This requires careful integration between ThreadX's TCB and the fake TCB used by
`rtos_int_enter`/`rtos_int_exit`.

### Files Changed

| File | Change |
|------|--------|
| `components/freertos/threadx/include/FreeRTOSConfig.h` | `portYIELD_FROM_ISR`/`portEND_SWITCHING_ISR` → `((void)0)` |
| `components/freertos/threadx/include/freertos/portmacro.h` | `#undef` + redefine to `((void)0)` |

---

## Bug 36 — `_tx_thread_system_state` Not Managed During ESP-IDF ISRs

**Iteration**: 31
**Date**: 2026-02-27

### Symptom

After fixing Bug 35, system still freezes after tick 32-33 when first WiFi interrupt
fires. Both threads print once then stop — timer tick appears to stop entirely.

### Investigation

Verified via agent search that ESP-IDF's `rv_utils_intr_enable/disable` do NOT clear
`mie` bit 17 — they only manipulate `PLIC_MXINT_ENABLE_REG`. Our timer hardware
configuration is preserved.

The actual issue was `_tx_thread_system_state` — ThreadX's ISR context indicator.

### Root Cause

`rtos_int_hooks.S` managed `port_uxInterruptNesting` (FreeRTOS compat counter) but
never touched `_tx_thread_system_state` (ThreadX's own ISR counter).

When a WiFi ISR called `xSemaphoreGive` → compat layer → `tx_semaphore_put` →
`_tx_thread_system_resume`, ThreadX checked `_tx_thread_system_state`:

- Value was `0` → ThreadX thought it was in thread context
- Called `_tx_thread_system_return()` for immediate preemption
- This saved the ISR stack pointer as the thread's stack pointer
- Scheduler corruption → system hang

The ThreadX timer path (vector[17]) correctly manages `_tx_thread_system_state` via
`_tx_thread_context_save` (increment) and `_tx_thread_context_restore` (decrement).
The ESP-IDF path (vector[N≠17]) is completely independent and needed its own
increment/decrement.

### Fix

Added to `rtos_int_hooks.S`:

**In `rtos_int_enter`** — increment `_tx_thread_system_state` alongside
`port_uxInterruptNesting`:
```asm
    .extern _tx_thread_system_state

    la      t3, _tx_thread_system_state
    lw      t5, 0(t3)
    addi    t5, t5, 1
    sw      t5, 0(t3)
```

**In `rtos_int_exit`** — decrement `_tx_thread_system_state` alongside
`port_uxInterruptNesting`:
```asm
    la      t2, _tx_thread_system_state
    lw      t4, 0(t2)
    beqz    t4, .Lskip_sys_dec
    addi    t4, t4, -1
    sw      t4, 0(t2)
.Lskip_sys_dec:
```

Nesting is correct for both same-path and cross-path scenarios:
- ESP-IDF only: 0→1 (enter), 1→0 (exit)
- ThreadX nests into ESP-IDF: 0→1 (rtos_enter), 1→2 (ctx_save), 2→1 (ctx_restore), 1→0 (rtos_exit)

### Result

WiFi scanning confirmed working. 17 networks detected. Both ThreadX threads
running continuously alongside WiFi ISR processing.

### Files Changed

| File | Change |
|------|--------|
| `components/freertos/threadx/src/rtos_int_hooks.S` | Added `_tx_thread_system_state` increment/decrement in `rtos_int_enter`/`rtos_int_exit` |

---

## Known Issue — Blocking WiFi Scan Timeout

**Iteration**: 31
**Date**: 2026-02-27
**Status**: Open (workaround in place)

### Symptom

`esp_wifi_scan_start(&config, true)` (blocking mode) times out after ~12 seconds
with `ESP_ERR_WIFI_TIMEOUT`. The scan hardware works — non-blocking mode returns
correct results.

### Workaround

Use non-blocking scan with a sleep:
```c
esp_wifi_scan_start(&scan_config, false);   // non-blocking
tx_thread_sleep(500);                        // 5 seconds
esp_wifi_scan_get_ap_num(&ap_count);         // collect results
esp_wifi_scan_get_ap_records(&count, recs);
```

### Likely Root Cause

The blocking path internally waits on a semaphore posted when `WIFI_EVENT_SCAN_DONE`
is processed through the event loop chain:

```
WiFi HW scan done → WiFi ISR → WiFi task → esp_event_post(SCAN_DONE)
  → event loop task → internal handler → xSemaphoreGive(scan_done_sem)
  → esp_wifi_scan_start unblocks
```

The semaphore never gets posted, so the notification is lost somewhere in this chain.
Likely candidates for investigation:
- `xQueueSend`/`xQueueReceive` in event loop task (queue compat correctness)
- Internal WiFi default event handler dispatch (registration at `esp_wifi_init` time)
- Semaphore give/take across task boundaries

---

## Bug 37 — ESP-IDF ISR Exit Never Triggers ThreadX Context Switch (esp_timer Broken)

**Iteration**: 32
**Date**: 2026-02-27

### Symptom

WiFi scan reports only channel 1 networks. `esp_timer` periodic callbacks never fire.
Diagnostic timer (`diag_timer_cb`, 2-second period) never prints after initial log.

WiFi scanning relies on `esp_timer` for per-channel dwell timing:
```
esp_wifi_scan_start → hardware tunes to channel 1 → esp_timer fires after dwell time
  → WiFi driver moves to channel 2 → esp_timer fires → channel 3 → ... → channel 13
  → WIFI_EVENT_SCAN_DONE
```

Without esp_timer callbacks, the scan stays on channel 1 forever.

### How to Investigate This (Reasoning Chain)

This section explains the step-by-step reasoning process for debugging this class of
issue — "a task's callback never runs even though its interrupt fires." If you encounter
a similar problem, follow these steps.

#### Step 1: Confirm the Hardware Interrupt Fires

Check if the peripheral interrupt reaches the CPU:

```
[diag] INTMTX src59 (esp_timer) → CPU line 5
[diag] PLIC ENABLE = 0x000603e2 (line 5 bit = 1)
[diag] mie = 0x00000888 (line 5 bit = ?)
[diag] SYSTIMER INT_ENA = 0x00000007 (bit2=alarm2=1)
```

What to check at each layer (see `docs/implementation/hardware-interrupts.md`):

| Layer | Register | What to verify |
|-------|----------|----------------|
| Peripheral | SYSTIMER INT_ENA (0x6000A064) | Bit for alarm2 (bit 2) = 1 |
| INTMTX | 0x60010000 + source*4 | Source 59 mapped to a CPU line |
| PLIC | ENABLE (0x20001000) | Bit for that CPU line = 1 |
| PLIC | Priority (0x20001010 + line*4) | Priority > threshold |
| CPU | mie CSR | Bit 11 (MEIE) = 1 |
| CPU | mstatus CSR | Bit 3 (MIE) = 1 when thread runs |

If all layers are configured, the interrupt IS reaching the CPU. The problem is
downstream — in the ISR dispatch or context switch path.

#### Step 2: Trace the ISR Dispatch Path

ESP32-C6 uses vectored interrupts. CPU line N jumps to `mtvec_base + N*4`.

Our vector table (`tx_initialize_low_level.S`) has two kinds of entries:
```
vector[0]  = j _tx_esp32c6_exception_handler    ← exceptions (ecall, fault)
vector[17] = j _tx_esp32c6_trap_handler          ← ThreadX timer (SYSTIMER alarm 0)
vector[N]  = j _interrupt_handler                ← ALL OTHER interrupts (ESP-IDF path)
```

**esp_timer uses SYSTIMER alarm 2 → source 59 → CPU line 5 → vector[5] → `_interrupt_handler`**

This means esp_timer goes through the ESP-IDF interrupt path, NOT the ThreadX timer path.

#### Step 3: Understand the ESP-IDF Interrupt Path

`_interrupt_handler` is defined in `esp-idf/components/riscv/vectors.S`. Its flow:

```
_interrupt_handler:
    save_general_regs            ← saves ALL 31 registers to stack (RV_STK_* offsets)
    mv   s1, mcause             ← save mcause in callee-saved register
    mv   s2, mstatus            ← save mstatus in callee-saved register
    call rtos_int_enter          ← OUR HOOK: manage nesting, switch to ISR stack
    mv   s4, a0                 ← save "context" return value (0 on C6)

    li   t0, <PLIC_MX_THRESH>
    lw   s3, (t0)              ← save old PLIC threshold
    sw   <high>, (t0)          ← raise threshold (mask lower-priority interrupts)

    csrsi mstatus, 0x8         ← ENABLE MIE — allows higher-priority interrupts to nest

    mv   a0, sp                ← arg0 = stack pointer (context frame)
    mv   a1, s1                ← arg1 = mcause (interrupt number)
    call _global_interrupt_handler  ← dispatch to registered handler (esp_timer ISR)

    csrci mstatus, 0x8         ← DISABLE MIE — prevent interrupts during exit
    sw   s3, (t0)              ← restore PLIC threshold

    mv   a0, s2                ← arg0 = saved mstatus (for rtos_int_exit)
    mv   a1, s4                ← arg1 = context (unused)
    call rtos_int_exit           ← OUR HOOK: manage nesting, check for context switch

    csrw mstatus, a0           ← restore mstatus (a0 = return from rtos_int_exit)
    restore_general_regs        ← restore all 31 registers
    mret                        ← return from interrupt
```

Key observations:
- `s1`, `s2`, `s3`, `s4` are callee-saved registers — they survive function calls
- After `_global_interrupt_handler` dispatches the ISR (e.g., esp_timer's handler), the
  ISR handler may call `xSemaphoreGive` → compat layer → `tx_semaphore_put` to wake up
  the esp_timer task
- `rtos_int_exit` is called AFTER the ISR handler returns — this is where we decide
  whether to context-switch to the newly-woken thread

#### Step 4: Understand What `tx_semaphore_put` Does During an ISR

When esp_timer's ISR handler calls `xSemaphoreGive(notification_sem)`:

```
xSemaphoreGive → xQueueGenericSend → tx_semaphore_put(&sem)
```

Inside `tx_semaphore_put` (file: `common/src/tx_semaphore_put.c`):
```c
// If a thread is waiting on this semaphore:
_tx_thread_preempt_disable++;          // Prevent preemption during resume
_tx_thread_system_resume(thread_ptr);  // Make the thread ready
_tx_thread_preempt_disable--;          // Allow preemption again
```

Inside `_tx_thread_system_resume` (file: `common/src/tx_thread_system_resume.c`):
```c
// Line ~302 — THIS IS THE KEY LINE:
_tx_thread_execute_ptr = thread_ptr;   // ALWAYS updated if higher priority
// ... but actual preemption check is:
if ((_tx_thread_system_state == 0) && (_tx_thread_preempt_disable == 0)) {
    _tx_thread_system_return();        // Only preempts in thread context
}
```

During an ISR:
- `_tx_thread_system_state > 0` (we incremented it in rtos_int_enter — Bug 36)
- So `_tx_thread_system_return()` is NOT called here
- BUT `_tx_thread_execute_ptr` IS updated to point to the higher-priority thread
- The execute_ptr update is "latched" — it persists after the ISR returns

**This means: after the ISR, `_tx_thread_execute_ptr ≠ _tx_thread_current_ptr`.**
Someone needs to check this and trigger a context switch.

#### Step 5: Identify What Was Missing (The Bug)

The OLD `rtos_int_exit` code did this when exiting the last ISR nesting level:

```asm
    /* OLD CODE — BROKEN */
    lw      t0, xPortSwitchFlag         ← check FreeRTOS switch flag
    beqz    t0, .Lno_switch             ← if 0, skip context switch
    sw      zero, xPortSwitchFlag       ← clear flag
    call    vTaskSwitchContext           ← call context switch
    lw      t0, pxCurrentTCBs           ← load current TCB
    lw      sp, 0(t0)                   ← restore SP from TCB
.Lno_switch:
    ...
```

THREE problems:
1. **`xPortSwitchFlag` was never set.** `portYIELD_FROM_ISR(x)` was defined as
   `((void)0)` — a no-op. In FreeRTOS, `portYIELD_FROM_ISR` sets this flag, but
   in our ThreadX compat layer, we correctly made it a no-op because calling
   `vPortYield()` from ISR context corrupts ThreadX (Bug 35). Nobody else sets it.

2. **`vTaskSwitchContext` was an empty stub.** Even if the flag were set, the function
   did nothing. It existed only as a linker symbol.

3. **`pxCurrentTCBs` was always NULL.** We never update this variable for ThreadX
   threads (it's used by ESP-IDF's `vectors.S` for stack switching in `rtos_int_enter`,
   but our current implementation doesn't maintain it for context switch purposes).

**Result**: `rtos_int_exit` NEVER performed a context switch. When `tx_semaphore_put`
woke up the esp_timer task during the WiFi ISR, `_tx_thread_execute_ptr` was updated
to point to the esp_timer task, but nobody checked it. The original thread continued
running after `mret`, and the esp_timer task only ran when the ThreadX timer tick
(vector[17]) happened to check `execute_ptr` in `_tx_thread_context_restore` — which
was at most every 10ms (100 Hz tick rate). For time-critical operations like WiFi
channel dwell timing, this delay was catastrophic.

#### Step 6: Understand How FreeRTOS Solves This (For Reference)

In real FreeRTOS on ESP32-C6:
```
ISR handler → xSemaphoreGiveFromISR(&sem, &xHigherPriorityTaskWoken)
  → if xHigherPriorityTaskWoken: portYIELD_FROM_ISR(xHigherPriorityTaskWoken)
  → sets xPortSwitchFlag = 1

rtos_int_exit:
  → sees xPortSwitchFlag == 1
  → calls vTaskSwitchContext() → updates pxCurrentTCB
  → restores SP from new pxCurrentTCB → mret into higher-priority task
```

We can't use this approach because ThreadX and FreeRTOS have fundamentally different
context management. FreeRTOS saves/restores ALL registers in a single flat frame.
ThreadX uses two different frame formats (solicited = callee-saved only, interrupt =
all registers). Trying to mix them would corrupt the stack.

### Root Cause

`rtos_int_exit` had no working mechanism to trigger a ThreadX context switch when
exiting ESP-IDF interrupt handlers. The `xPortSwitchFlag + vTaskSwitchContext`
mechanism was a FreeRTOS concept that doesn't apply to ThreadX.

### Fix

Replace the entire preemption mechanism in `rtos_int_exit` with a call to ThreadX's
own `_tx_thread_system_preempt_check()`.

**New `rtos_int_exit` (annotated assembly)**:

```asm
rtos_int_exit:
    /* ── Save mstatus in callee-saved register ──────────────────────
     * a0 = saved mstatus from _interrupt_handler (passed via s2).
     * s11 is callee-saved — it survives ALL function calls, including
     * _tx_thread_system_return() and the entire scheduler round-trip.
     * When the original thread is eventually re-scheduled via
     * _tx_thread_synch_return, s11 is restored, so we get our mstatus back. */
    mv      s11, a0

    /* ── Check if scheduler is running ────────────────────────────── */
    lw      t0, port_xSchedulerRunning
    beqz    t0, .Lexit_end              /* Pre-scheduler: return mstatus */

    /* ── Decrement ISR nesting counter (FreeRTOS compat) ──────────── */
    la      t2, port_uxInterruptNesting
    lw      t3, 0(t2)
    beqz    t3, .Lskip_dec              /* Defensive: already 0 */
    addi    t3, t3, -1
    sw      t3, 0(t2)
.Lskip_dec:

    /* ── Decrement ThreadX system state (Bug 36 fix) ──────────────── */
    la      t2, _tx_thread_system_state
    lw      t4, 0(t2)
    beqz    t4, .Lskip_sys_dec          /* Defensive: already 0 */
    addi    t4, t4, -1
    sw      t4, 0(t2)
.Lskip_sys_dec:

    /* ── If still nested, skip preemption check ───────────────────── */
    bnez    t3, .Lexit_end              /* t3 = port_uxInterruptNesting after dec */

    /* ══════════════════════════════════════════════════════════════════
     * EXITING LAST ISR LEVEL — THIS IS WHERE THE BUG 37 FIX LIVES
     *
     * _tx_thread_system_preempt_check() does three things:
     *   1. Checks _tx_thread_system_state == 0   (we just decremented to 0)
     *   2. Checks _tx_thread_preempt_disable == 0 (balanced by tx_semaphore_put)
     *   3. Compares _tx_thread_execute_ptr != _tx_thread_current_ptr
     *
     * If all conditions are met, it calls _tx_thread_system_return() which:
     *   a) Allocates 16*REGBYTES on the CURRENT stack
     *   b) Stores type=0 (solicited frame marker)
     *   c) Saves ra, s0-s11, mstatus (callee-saved regs)
     *   d) Saves SP into current thread's TCB
     *   e) Switches to system stack
     *   f) Clears _tx_thread_current_ptr
     *   g) Jumps to _tx_thread_schedule
     *
     * The scheduler then:
     *   - Waits for _tx_thread_execute_ptr != NULL (already set)
     *   - Sets _tx_thread_current_ptr = execute_ptr
     *   - Loads new thread's SP from its TCB
     *   - Checks stack type at offset 0
     *     - Type 1 (interrupt frame): full register restore + mret
     *     - Type 0 (solicited frame): restore callee-saved regs + ret
     *   - The higher-priority thread runs
     *
     * When the higher-priority thread finishes (or sleeps/blocks):
     *   - Scheduler picks our original thread again
     *   - Loads our SP (which points to the solicited frame we saved above)
     *   - _tx_thread_synch_return restores ra, s0-s11, mstatus
     *   - Including s11 = our saved mstatus!
     *   - `ret` returns to the `lw ra, 0(sp)` instruction below
     *   - We continue exiting as if the preemption never happened
     * ══════════════════════════════════════════════════════════════════ */
    addi    sp, sp, -16
    sw      ra, 0(sp)
    call    _tx_thread_system_preempt_check
    lw      ra, 0(sp)
    addi    sp, sp, 16

.Lexit_end:
    mv      a0, s11                     /* Return mstatus to _interrupt_handler */
    ret
```

**Why `_tx_thread_system_preempt_check` is the right solution**:

1. **It's ThreadX's own mechanism.** No need to bridge between FreeRTOS and ThreadX
   concepts. ThreadX already tracks which thread should run next (`_tx_thread_execute_ptr`)
   and knows how to save/restore context.

2. **It handles the solicited/interrupt frame distinction correctly.**
   `_tx_thread_system_return()` saves a type-0 (solicited) frame with only callee-saved
   registers. This is correct because we're in a function call context (C calling
   convention applies), not in an interrupt context. The interrupt context is managed
   by `_interrupt_handler` in `vectors.S` (the ESP-IDF code above/below us).

3. **Callee-saved registers bridge the gap.** The register `s11` holds our saved
   `mstatus` value. When `_tx_thread_system_return()` saves the solicited frame, it
   stores s11 at offset `1*REGBYTES(sp)`. When the thread resumes via
   `_tx_thread_synch_return`, s11 is restored from that offset. So our mstatus value
   survives the entire scheduler round-trip.

4. **The return path is transparent.** After `_tx_thread_synch_return` does `ret`,
   control returns to the instruction after `call _tx_thread_system_preempt_check`.
   We pop `ra`, return mstatus in `a0`, and `rtos_int_exit` returns to
   `_interrupt_handler`. From `_interrupt_handler`'s perspective, `rtos_int_exit`
   just returned normally — it has no idea that the scheduler ran in between.

### Stack Frame Anatomy During Preemption

When preemption happens, there are THREE nested stack frames on the original thread's
stack:

```
Higher addresses (stack top)
┌──────────────────────────────────────────────┐
│ _interrupt_handler's saved registers          │ ← 32 registers (RV_STK_* offsets)
│ (saved by save_general_regs macro in vectors.S)│    This is the interrupt frame
│ Includes: ra, t0-t6, a0-a7, s0-s11, mepc    │    that will be restored by
│                                              │    restore_general_regs + mret
├──────────────────────────────────────────────┤
│ rtos_int_exit's ra save                       │ ← 16 bytes (for alignment)
│ sw ra, 0(sp)                                 │    Pop'd after preempt_check returns
├──────────────────────────────────────────────┤
│ _tx_thread_system_return's solicited frame    │ ← 16*REGBYTES (64 bytes on RV32)
│ Offset 0:           type = 0 (solicited)     │    Stored by system_return
│ Offset 1*REGBYTES:  s11 (= saved mstatus)   │    Restored by synch_return
│ Offset 2*REGBYTES:  s10                      │
│ ...                                          │
│ Offset 12*REGBYTES: s0                       │
│ Offset 13*REGBYTES: ra                       │
│ Offset 14*REGBYTES: mstatus                  │
├──────────────────────────────────────────────┤
│ (system stack — scheduler runs here)          │
└──────────────────────────────────────────────┘
Lower addresses (stack grows down)
```

`_tx_thread_system_return` saves SP (pointing to the solicited frame) into the thread's
TCB at `tx_thread_stack_ptr` (offset 2*REGBYTES). When the thread is re-scheduled,
`_tx_thread_schedule` loads this SP, sees type=0 at offset 0, and jumps to
`_tx_thread_synch_return` which restores s0-s11, ra, mstatus and does `ret`.

### Complete Flow: WiFi ISR → esp_timer Task Runs → Original Thread Resumes

```
1. WiFi hardware fires interrupt → INTMTX routes to CPU line 5
2. CPU vectors to _interrupt_handler (vector[5])
3. _interrupt_handler:
   a. save_general_regs (all 31 regs onto task stack)
   b. call rtos_int_enter → system_state++ (now 1), switch to ISR stack
   c. csrsi mstatus, 0x8 (enable nested interrupts)
   d. call _global_interrupt_handler → dispatches WiFi ISR handler
      WiFi ISR handler:
        → xSemaphoreGive(notification) → tx_semaphore_put
          → _tx_thread_system_resume(esp_timer_task)
            → _tx_thread_execute_ptr = esp_timer_task  [LATCHED]
            → system_state > 0, so NO immediate preempt
   e. csrci mstatus, 0x8 (disable interrupts for exit)
   f. call rtos_int_exit:
      → system_state-- (now 0), nesting-- (now 0)
      → last ISR level → call _tx_thread_system_preempt_check
        → system_state==0 ✓, preempt_disable==0 ✓
        → execute_ptr (esp_timer) != current_ptr (main_thread) ✓
        → call _tx_thread_system_return:
          → save solicited frame (type=0, s0-s11, ra, mstatus)
          → TCB->stack_ptr = sp
          → switch to system stack, clear current_ptr
          → jump to _tx_thread_schedule
            → pick esp_timer_task from execute_ptr
            → set current_ptr = esp_timer_task
            → load esp_timer_task's SP
            → restore esp_timer_task's context → esp_timer_task RUNS
              esp_timer task:
                → calls diag_timer_cb("esp_timer fired!")
                → calls WiFi channel-advance callback
                → blocks on next notification (tx_semaphore_get)
            → scheduler picks original thread again
            → load original thread's SP (points to solicited frame)
            → _tx_thread_synch_return:
              → restore s0-s11 (including s11 = saved mstatus)
              → restore ra, mstatus
              → ret → back to instruction after call _tx_thread_system_preempt_check
      → lw ra, 0(sp); addi sp, sp, 16
      → mv a0, s11 (mstatus)
      → ret → back to _interrupt_handler
   g. csrw mstatus, a0 (restore mstatus with MIE=0)
   h. restore_general_regs (all 31 regs from task stack)
   i. mret → resumes original thread where it was interrupted
```

### Result

- `esp_timer` callbacks now fire immediately when their ISR runs (not delayed until
  the next 10ms ThreadX timer tick)
- WiFi scanning works across all 13 channels
- Both wifi_demo and basic ThreadX demo build and link correctly

### Files Changed

| File | Change |
|------|--------|
| `components/freertos/threadx/src/rtos_int_hooks.S` | Replaced `xPortSwitchFlag + vTaskSwitchContext` with `_tx_thread_system_preempt_check()` call in `rtos_int_exit` |
| `components/freertos/threadx/include/freertos/portmacro.h` | Updated `portYIELD_FROM_ISR` comment to explain new mechanism |
| `components/freertos/threadx/src/port.c` | Updated `vTaskSwitchContext` comment (now linker stub only) |

### Key Source Files for Further Study

| File | What it does |
|------|-------------|
| `esp-idf/components/riscv/vectors.S` | `_interrupt_handler`: ESP-IDF's ISR entry/exit wrapper |
| `esp-idf/components/riscv/interrupt.c` | `_global_interrupt_handler`: ISR dispatch table lookup |
| `threadx/common/src/tx_thread_system_preempt_check.c` | Checks if context switch needed |
| `threadx/common/src/tx_thread_system_resume.c` | Makes thread ready, updates `execute_ptr` |
| `threadx/ports/risc-v64/gnu/src/tx_thread_system_return.S` | Saves solicited frame, enters scheduler |
| `threadx/ports/risc-v64/gnu/src/tx_thread_schedule.S` | Scheduler loop + `_tx_thread_synch_return` |
| `threadx/common/src/tx_semaphore_put.c` | Wakes waiting thread with preempt_disable guard |

---

## Bug 38 — `_tx_thread_system_preempt_check()` in rtos_int_exit Creates Solicited Frame on ISR Stack

**Phase**: After Bug 37 fix (rtos_int_exit called `_tx_thread_system_preempt_check`)

**Symptom**: System hung at `esp_wifi_start()`. The `_tx_thread_system_preempt_check()`
approach from Bug 37 created a "solicited" context frame (type=0) on the ISR stack.
ESP-IDF's `_interrupt_handler` expects an "interrupt" frame (type=1) with a completely
different layout and size. When the interrupt handler tried to restore registers from
the corrupted frame, execution went to a garbage address.

**Root cause**: `_tx_thread_system_preempt_check()` calls `_tx_thread_system_return()`
when a preemption is needed. `_tx_thread_system_return` saves callee-saved registers
(s0-s11, ra, mstatus) in a 16*REGBYTES solicited frame on the **current stack** (which
is the ISR stack during an ESP-IDF interrupt). It then enters `_tx_thread_schedule` which
dispatches the higher-priority thread. When that thread eventually suspends and the
preempted thread resumes, ThreadX restores the solicited frame.

But the ESP-IDF `_interrupt_handler` doesn't know about this frame. After `rtos_int_exit`
returns, `_interrupt_handler` does `csrw mstatus, a0` then `restore_general_regs` + `mret`.
The stack pointer is wrong (offset by the solicited frame size), so all register restores
read garbage values.

**Fix**: Reverted `rtos_int_exit` to the FreeRTOS pattern:

```asm
rtos_int_exit:
    mv      s11, a0                     /* Save mstatus */
    lw      t0, port_xSchedulerRunning
    beqz    t0, .Lexit_end

    /* Decrement nesting counters (FreeRTOS + ThreadX system_state) */
    la      t2, port_uxInterruptNesting
    lw      t3, 0(t2)
    addi    t3, t3, -1
    sw      t3, 0(t2)

    la      t2, _tx_thread_system_state  /* Bug 36 fix preserved */
    lw      t4, 0(t2)
    addi    t4, t4, -1
    sw      t4, 0(t2)

    bnez    t3, .Lexit_end              /* Still nested — keep ISR stack */

    /* Check xPortSwitchFlag → vTaskSwitchContext (dead code for now) */
    la      t0, xPortSwitchFlag
    lw      t2, 0(t0)
    beqz    t2, .Lno_switch
    call    vTaskSwitchContext           /* stub */
    sw      zero, 0(t0)

.Lno_switch:
    /* ALWAYS restore SP from pxCurrentTCBs at last ISR level */
    lw      t0, pxCurrentTCBs
    lw      sp, 0(t0)                   /* Restore task SP */

.Lexit_end:
    mv      a0, s11                     /* Return mstatus */
    ret
```

**Key lesson**: rtos_int_exit MUST NOT call ThreadX functions that create stack frames
(`_tx_thread_system_preempt_check`, `_tx_thread_system_return`). The ESP-IDF interrupt
handler's frame format is incompatible with ThreadX's solicited frame. The ISR exit
must be purely mechanical: decrement counters, restore SP, return mstatus.

**Note on preemption**: With this pattern, ISR-level preemption (where an ISR wakes a
higher-priority thread and it runs immediately) is NOT implemented. Preemption only
happens at the next ThreadX timer tick (vector[17]) when `_tx_thread_context_restore`
compares `_tx_thread_current_ptr` vs `_tx_thread_execute_ptr`. This is sufficient for
WiFi/BLE because their critical path latency requirements are in the millisecond range.

---

## Bug 39 — PLIC Threshold = 1 Masks ALL ESP-IDF Interrupts (esp_timer, WiFi, BLE)

**Phase**: WiFi scanning stuck on channel 1, esp_timer callbacks never fire

**Symptom**: WiFi scan started but SCAN_DONE event never delivered. esp_timer diagnostic
timer never fired. Only the ThreadX SYSTIMER tick (100 Hz) was working. Scan #1 returned
21 results (all channel 1) but the scan never progressed to channels 2-13.

**Root cause**: ESP-IDF startup sets PLIC threshold to 1 (`RVHAL_INTR_ENABLE_THRESH`).
`esp_intr_alloc()` assigns priority 1 by default. The PLIC only delivers interrupts with
priority **strictly greater than** the threshold. With threshold=1, priority 1 interrupts
are masked (1 > 1 = false). This means ALL ESP-IDF interrupts (esp_timer, WiFi, BLE,
UART, etc.) are permanently silenced.

Real FreeRTOS handles this by using `esprv_intc_int_set_threshold(0)` as its
`portENABLE_INTERRUPTS()` (lowering threshold to 0 when interrupts should be enabled).
Our port uses `csrs mstatus, 8` (toggle MIE bit) for portENABLE/DISABLE and never
touches the PLIC threshold.

Our SYSTIMER uses priority 2 (> threshold 1), so the tick always worked. But everything
from `esp_intr_alloc` was dead.

**Evidence**: `PLIC THRESH = 0x00000001` in diagnostics, confirmed with readback.

**Initial fix**: Added `PLIC_MX_THRESH = 0` in `_tx_port_setup_timer_interrupt`.

**Revised fix (Bug 40)**: Moved PLIC threshold setting out of shared timer code into
the custom FreeRTOS component via weak/strong `_tx_port_esp_idf_isr_init()`. See Bug 40.

---

## Bug 40 — threadx_demo Hangs After WiFi Port Restructuring

**Phase**: After adding weak symbols to tx_port_startup.c and PLIC threshold fix (Bug 39)

**Symptom**: threadx_demo output ends at "FreeRTOS compat layer initialized". No thread
output, no tick count, complete hang. Diagnostic shows:
- `mstatus = 0x00000001` (MIE=0, correct during init)
- `PLIC THRESH = 0x00000000` (Bug 39 fix applied)
- `mip = 0x00020000` (bit 17 pending — SYSTIMER ready)
- `mie = 0x0a020924` (MEIE + line 17 + ESP-IDF lines)

**Root cause**: Two changes in `tx_port_startup.c` that are ONLY appropriate for the
wifi_demo were applied unconditionally to both demos:

```c
pxCurrentTCBs = &s_current_task_sp_save;   // fake TCB for ISR stack switching
port_xSchedulerRunning = 1u;               // enables ISR stack switching
```

In the **threadx_demo** (which links the REAL ESP-IDF FreeRTOS component, NOT our custom
override in `components/freertos/`), these symbols resolve to the real FreeRTOS variables:

| Symbol | threadx_demo resolves to | wifi_demo resolves to |
|--------|-------------------------|----------------------|
| `port_xSchedulerRunning` | Real FreeRTOS BSS (portasm.S) | Our rtos_int_hooks.S `.data` |
| `pxCurrentTCBs` | Real FreeRTOS BSS (portasm.S) | Our rtos_int_hooks.S `.data` |

Setting `port_xSchedulerRunning = 1` in the threadx_demo tells the REAL FreeRTOS
`rtos_int_enter`/`rtos_int_exit` (from ESP-IDF's portasm.S) that the scheduler is running.
Combined with `PLIC_MX_THRESH = 0` (which allows ESP-IDF interrupts like the interrupt
watchdog to fire), the real FreeRTOS ISR hooks attempt stack switching with our fake TCB.

The real FreeRTOS hooks do NOT manage `_tx_thread_system_state` (that's only in our
custom `rtos_int_hooks.S`). So any ESP-IDF interrupt going through the real FreeRTOS path
leaves ThreadX's ISR nesting state inconsistent — ThreadX doesn't know an ISR is active,
and kernel APIs may attempt illegal operations.

Additionally, if the SYSTIMER interrupt (vector[17]) nests inside a real-FreeRTOS-handled
ESP-IDF interrupt, the ThreadX `_tx_thread_context_restore` idle path jumps directly to
`_tx_thread_schedule`, abandoning the outer ESP-IDF interrupt's stack frame entirely.

**Contrast with old working state**: Before the WiFi port, the threadx_demo had:
- `port_xSchedulerRunning = 0` (real FreeRTOS default — no ISR stack switching)
- `PLIC threshold = 1` (only SYSTIMER at priority 2 fires, no ESP-IDF interrupts)
- No ESP-IDF interrupt interference whatsoever

**Fix**: Separated ISR initialization into a weak/strong function pattern:

1. **`tx_port_startup.c`** (shared threadx component):
   - Removed `port_xSchedulerRunning`, `pxCurrentTCBs`, `s_current_task_sp_save` declarations
   - Added weak noop: `__attribute__((weak)) void _tx_port_esp_idf_isr_init(void) { }`
   - Calls `_tx_port_esp_idf_isr_init()` in `tx_application_define` (after compat layer init)

2. **`tx_esp32c6_timer.c`** (shared threadx component):
   - Removed `PLIC_MX_THRESH = 0` — timer only configures SYSTIMER (priority 2 > default threshold 1)
   - Timer code is now decoupled from port-level interrupt policy

3. **`port.c`** (custom FreeRTOS component — wifi_demo only):
   - Added strong override: `void _tx_port_esp_idf_isr_init(void)` that sets
     `PLIC_MX_THRESH = 0` and `port_xSchedulerRunning = 1`

**Result** (verified via symbol table):

| Binary | `_tx_port_esp_idf_isr_init` | `port_xSchedulerRunning` | `pxCurrentTCBs` |
|--------|---------------------------|-------------------------|----------------|
| threadx_demo | W (weak noop) | B (BSS=0, real FreeRTOS) | B (BSS=0, real FreeRTOS) |
| wifi_demo | T (strong, sets thresh+flag) | D (our rtos_int_hooks.S) | D (points to save area) |

- **threadx_demo**: PLIC threshold stays 1, only SYSTIMER fires, no ISR stack switching. **Fixed — boots and runs.**
- **wifi_demo**: PLIC threshold 0, all ESP-IDF interrupts fire, ISR stack switching active. **Builds successfully — WiFi scanning issue still present (same as before Bug 40).**

### Files Changed

| File | Change |
|------|--------|
| `components/threadx/port/tx_port_startup.c` | Removed port_xSchedulerRunning/pxCurrentTCBs setup; added weak `_tx_port_esp_idf_isr_init()` |
| `components/threadx/port/tx_esp32c6_timer.c` | Removed PLIC_MX_THRESH=0; renumbered steps 4a-4e → 3a-3e |
| `components/freertos/threadx/src/port.c` | Added strong `_tx_port_esp_idf_isr_init()` with PLIC threshold + scheduler flag |

### Key Lesson

Port-specific ISR integration (PLIC threshold, ISR stack switching flags) must live in the
component that owns the ISR hooks, not in the shared threadx kernel component. When multiple
demos link against different FreeRTOS implementations (real vs custom), setting symbols in
shared code writes to different underlying storage with completely different semantics.

---

## WiFi Demo — Remaining Issue (Not Yet Resolved)

**Status**: WiFi scanning stuck on channel 1 / esp_timer not firing. The PLIC threshold
fix (Bug 39) is correctly applied via `_tx_port_esp_idf_isr_init()`, but the underlying
WiFi scanning issue persists. This is a separate problem from the threadx_demo hang
(Bug 40) and requires further investigation.

**Symptoms observed**:
- WiFi starts successfully (past "wifi:enable tsf")
- Scan #1 returns 21 results (all channel 1)
- Scans 2-3 return 0 results
- esp_timer diagnostic timer never fires
- SCAN_DONE event appears to be delivered for scan #1 but subsequent scans fail

**Possible next steps for investigation**:
1. Verify PLIC threshold is actually 0 at runtime (add diagnostic readback in app_main)
2. Check if esp_timer interrupt is actually allocated and its PLIC line is enabled
3. Verify `_tx_thread_system_state` tracking is correct during WiFi ISRs (Bug 36 fix)
4. Check if the rtos_int_exit SP restore from pxCurrentTCBs is corrupting ThreadX thread state
5. Consider whether portYIELD_FROM_ISR being a no-op prevents timely ISR→thread notification

---

## Bug 41 — Pre-Kernel Task Orphaning: esp_timer / WiFi Scan Never Complete

**Phase**: WiFi demo — esp_timer callbacks never fire, all scan results on channel 1

**Symptom**:
- esp_timer diagnostic timer (created in `app_main`) fires zero times despite being started
- WiFi scan #1 returns results but all APs on channel 1 only
- Scans 2+ return 0 AP records
- `SCAN_DONE` event never received for scans 2+

**Root cause**: ESP-IDF secondary initialization (`do_secondary_init()`) runs **before**
`esp_startup_start_app()` and therefore before `tx_kernel_enter()`. Functions registered
via `ESP_SYSTEM_INIT_FN(..., SECONDARY, ...)` — most critically `esp_timer_init_os()` —
call `xTaskCreate()` at this early stage, creating FreeRTOS (compat-layer) tasks.

When `tx_kernel_enter()` runs later, it calls `_tx_thread_initialize()` internally.
This function clears ThreadX's global scheduler bookkeeping:

```c
_tx_thread_created_ptr = TX_NULL;
TX_MEMSET(_tx_thread_priority_list, 0, sizeof(_tx_thread_priority_list));
TX_MEMSET(_tx_thread_priority_maps, 0, sizeof(_tx_thread_priority_maps));
```

These pre-kernel tasks are now **orphaned**: their `txfr_task_t` structs exist in memory
and their stacks are allocated, but they are absent from every priority queue. When the
esp_timer ISR calls `tx_semaphore_put(&notification_sem)` the semaphore count increments,
but no thread ever wakes because the task was never placed in the ready list after the
kernel clear.

**Why channel 1 specifically**: The esp_timer task processes internal WiFi scan timeout
callbacks that advance the scan from one channel to the next. Without it running, the
hardware completes its dwell time on channel 1 and signals the driver, but the driver
never instructs the hardware to move to channel 2. Any APs found during the channel-1
dwell appear in the results; everything else is invisible.

**Affected tasks** created by ESP-IDF secondary init (before `tx_kernel_enter()`):
- `esp_timer` task — `esp_timer_init_os()` → `xTaskCreate("esp_timer", ...)`
- Potentially `tiT` (IDF timer daemon), newlib lock tasks, and others per configuration

**Fix**: Pre-kernel task tracking + re-registration in `tx_freertos.c`:

1. **Tracking** (`xTaskCreate`): when `txfr_pre_kernel_tracking == 1u` (flag cleared only
   in `tx_freertos_init()`), save a pointer to each created `txfr_task_t` in a static array
   `txfr_pre_kernel_tasks[TXFR_MAX_PRE_KERNEL_TASKS]`.

2. **Re-registration** (`tx_freertos_init()`, called from `tx_application_define()` after
   `tx_kernel_enter()` completes):
   - Read stack parameters from the still-valid TCB fields (`tx_thread_stack_start`,
     `tx_thread_stack_size`, `tx_thread_name`, `tx_thread_priority`) — these survive
     `_tx_thread_initialize()` because it only zeroes the global list/map arrays, not
     individual `TX_THREAD` structs.
   - Clear `tx_thread_id = 0` so `tx_thread_create` accepts the already-allocated TCB.
   - Call `tx_thread_create(..., TX_DONT_START)` to re-register with the live kernel.
   - Clear `tx_semaphore_id = 0` and call `tx_semaphore_create` to re-register the
     notification semaphore.
   - Call `tx_thread_resume` to put the thread back into the ready queue.

3. **Guard**: All added code is wrapped in `#ifdef ESP_PLATFORM` to keep the upstream
   compat layer portable to non-ESP targets.

**Result**: esp_timer callbacks fire correctly. WiFi scan completes all channels. `SCAN_DONE`
received. APs reported on their real channels (not stuck on channel 1). Both `wifi_demo`
and `threadx_demo` build and run correctly.

### Files Changed

| File | Change |
|------|--------|
| `components/threadx/threadx/utility/rtos_compatibility_layers/FreeRTOS/tx_freertos.c` | Pre-kernel tracking globals + `xTaskCreate` tracking + `tx_freertos_init` re-registration loop |

### Key Lesson

ESP-IDF secondary init creates FreeRTOS tasks **before** `tx_kernel_enter()`. Any RTOS
that replaces FreeRTOS via a compat layer must account for this: tasks created before the
kernel starts will be orphaned when `_tx_thread_initialize()` clears the global scheduler
tables. The fix is to snapshot those tasks before the clear and re-register them into the
live kernel immediately after init completes.

---
