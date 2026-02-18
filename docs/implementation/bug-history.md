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

## Current Status (as of this writing)

Bugs 1–9 are confirmed fixed (build succeeds).

Bugs 10–18 are code fixes applied but **not yet verified on hardware** —
the session was interrupted before re-flashing. The fixes are logically sound
based on the register reference documentation.

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
