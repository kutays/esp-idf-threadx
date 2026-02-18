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

## Current Status (as of this writing)

Bugs 1–9: confirmed fixed (build succeeds, pre-hardware verification).
Bugs 10–18: HAL-based rewrites applied.
Bugs 19–25: fixed previous session. Build succeeds.
Bug 26: fixed this session — EIP_STATUS check removed from trap handler.
Ready for hardware test to confirm tick advances and `tx_thread_sleep` works.

## Remaining Questions for Audit

1. **mstatus.MIE timing**: Is `mstatus.MIE` set at the point we set `mie.MEIE`?
   The ESP-IDF startup code may have MIE cleared at this point — verify with JTAG.

2. **Counter unit 0 auto-start**: Does counter unit 0 start counting automatically
   when CLK_FO is set, or does it require the `TIMER_UNIT0_WORK_EN` bit (bit 30)
   of SYSTIMER_CONF_REG to be set? It appears to be on by default but should be verified.

3. **INTMTX reset state**: What CPU line does SYSTIMER_TARGET0 map to at reset?
   If it defaults to line 0, and line 0 has unexpected behavior, we may see issues.
   Verify by reading `0x600100E4` before and after our init.

4. **PLIC MX priority registers**: Our priority register formula is
   `0x20001010 + N*4`. Verify this matches the hardware by reading back the written
   value for line 7 (`0x2000102C`) after init.

5. **Exception handler**: The spin loop in `_handle_exception` silently halts
   the CPU on any fault. A production port should log the mcause and mepc and
   call a proper panic handler.
