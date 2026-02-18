# ESP32-C6 Interrupt Hardware

## Overview

The ESP32-C6 has a **three-layer** interrupt architecture, very different from
the standard RISC-V model (which uses a single PLIC + mtime):

```
Peripheral IRQ sources (100+)
         │
         ▼
  [1] Interrupt Matrix (INTMTX)    0x60010000
      Routes any source to any of 32 CPU lines
         │
         ▼
  [2] PLIC Machine-mode (PLIC MX)  0x20001000
      Controls enable/priority/type per CPU line
         │
         ▼
  [3] RISC-V mie CSR + mstatus.MIE
      CPU-level enable gates
         │
         ▼
      CPU core
```

Each layer must be properly configured. Missing any one layer = no interrupt.

---

## Layer 1: Interrupt Matrix (INTMTX)

**Base address**: `DR_REG_INTERRUPT_MATRIX_BASE = 0x60010000`

**Header**: `soc/esp32c6/register/soc/interrupt_matrix_reg.h`

The INTMTX contains one 32-bit map register per peripheral source.
Writing the desired CPU line number (0–31) to that register connects
the source to that CPU line.

```
Register address for source N:  0x60010000 + N * 4

SYSTIMER_TARGET0 is source 57:
  Address = 0x60010000 + 57*4 = 0x600100E4
  Write value = 7  (CPU interrupt line 7)
```

**Reference**: `soc/esp32c6/include/soc/interrupts.h`
```c
// Counted from the enum in that file:
ETS_SYSTIMER_TARGET0_INTR_SOURCE = 57
```

**Important**: The old code had `base + 0x80 + src*4` which is wrong.
The correct formula is simply `base + src*4`.

> **Update (Bug 27)**: CPU interrupt line 7 is CLINT-reserved and cannot be
> used. Write value changed from 7 to **17**. See "Reserved and Disabled CPU
> Interrupt Lines" section below.

---

## Layer 2: PLIC Machine-Mode (PLIC MX)

**Base address**: `DR_REG_PLIC_MX_BASE = 0x20001000`

**Header**: `soc/esp32c6/register/soc/plic_reg.h`

Note this is in a **different address space** from the main APB bus (0x6xxxxxxx).
It lives at 0x2xxxxxxx, in the CPU subsystem local address space.

### Register Map

| Offset | Address    | Name                  | Description                              |
|--------|------------|----------------------|------------------------------------------|
| +0x00  | 0x20001000 | PLIC_MXINT_ENABLE_REG | Bit N = 1 enables CPU interrupt line N  |
| +0x04  | 0x20001004 | PLIC_MXINT_TYPE_REG   | Bit N = 0: level-triggered, 1: edge     |
| +0x08  | 0x20001008 | PLIC_MXINT_CLEAR_REG  | Write bit N = 1 to clear edge latch     |
| +0x0C  | 0x2000100C | PLIC_EMIP_STATUS_REG  | Read-only: bit N = 1 if line N pending  |
| +0x10  | 0x20001010 | PLIC_MXINT0_PRI_REG   | Priority for line 0 (4-bit field)       |
| +0x14  | 0x20001014 | PLIC_MXINT1_PRI_REG   | Priority for line 1                     |
| ...    | ...        | ...                   | One register per line (32 total)        |
| +0x8C  | 0x2000108C | PLIC_MXINT31_PRI_REG  | Priority for line 31                    |
| +0x90  | 0x20001090 | PLIC_MXINT_THRESH_REG | Threshold: only fire if priority > this |
| +0x94  | 0x20001094 | PLIC_MXINT_CLAIM_REG  | Claim/complete register                 |
| +0x3FC | 0x200013FC | PLIC_MXINT_CONF_REG   | Configuration                           |

### Priority Formula

For CPU interrupt line N:
```
Priority register address = 0x20001000 + 0x10 + N * 4
                           = PLIC_MXINT0_PRI_REG + N * 4
```

Interrupt fires if: `priority[N] > threshold` AND `enable[N] = 1`

We use priority = 1, threshold = 0, so any enabled interrupt fires.

> **Update (Bug 27)**: FreeRTOS startup sets `PLIC_MXINT_THRESH_REG = 1`
> (constant `RVHAL_INTR_ENABLE_THRESH` in `hal/rv_utils.h`). Priority 1 ≤
> threshold 1 is masked. Changed to priority **2** (strictly greater than 1).
> The firing condition is also more nuanced — see "mie Bit N" in the Layer 3
> update below.

### Edge vs Level Triggered

Our SYSTIMER interrupt is configured as **edge-triggered** (TYPE bit = 1).
This means after the interrupt fires, we must explicitly clear the edge latch
by writing to PLIC_MXINT_CLEAR_REG before returning, otherwise the interrupt
fires again immediately.

### INTPRI vs PLIC MX

There is also a peripheral at `DR_REG_INTPRI_BASE = 0x600C5000` with similar
registers (INTPRI_CORE0_CPU_INT_ENABLE_REG, etc.). This is **NOT** the same
hardware as PLIC MX. For machine-mode operation (which ThreadX uses), you must
configure the PLIC MX registers at 0x20001000.

**Our previous bug**: We were writing to INTPRI (0x600C5000) instead of PLIC MX
(0x20001000). The interrupt never fired because the machine-mode CPU lines were
never enabled.

---

## Layer 3: RISC-V CSR Gates

Two CSR bits must both be set for any interrupt to reach the CPU:

### mstatus.MIE (bit 3)

Global machine interrupt enable. Set by ThreadX when threads are running.
Cleared by `csrci mstatus, 8` during critical sections.

ThreadX manages this automatically via `TX_DISABLE` / `TX_RESTORE` macros
and via the context restore path (mret restores mstatus from mepc/mstatus
saved on interrupt entry).

### mie.MEIE (bit 11)

Machine External Interrupt Enable. Must be set to allow the PLIC to deliver
interrupts to the CPU.

**Our previous bug**: ThreadX's port only manages mstatus.MIE, not mie.MEIE.
Without setting mie.MEIE, no external interrupt (including SYSTIMER) can fire,
even if mstatus.MIE is set and the PLIC is configured.

We set this in `_tx_initialize_low_level`:
```asm
li    t0, 0x800      /* bit 11 = MEIE */
csrs  mie, t0        /* set MEIE in mie CSR */
```

The `csrs` instruction atomically sets bits in a CSR:
- `csrs csr, rs1` — performs `CSR = CSR | rs1`

### Summary of CSR Bits Used

| CSR     | Bit | Name  | Set by us?      | Purpose                        |
|---------|-----|-------|-----------------|--------------------------------|
| mstatus | 3   | MIE   | ThreadX         | Global machine interrupt gate  |
| mie     | 11  | MEIE  | Yes, at init    | Machine external interrupt gate|
| mie     | 7   | MTIE  | Not used        | Machine timer (mtime) gate     |
| mie     | 3   | MSIE  | Not used        | Machine software interrupt gate|

---

## SYSTIMER Peripheral

**Reference**: `soc/esp32c6/register/soc/systimer_reg.h`

The SYSTIMER has:
- 2 free-running 52-bit counter units (unit 0 and unit 1)
- 3 comparators (COMP0, COMP1, COMP2) that can alarm against either unit
- Interrupt output per comparator

We use **COMP0** against **unit 0** (the default counter).

**Clock frequency**: 16 MHz from XTAL_CLK (fixed, does not scale with CPU clock).

### Tick Calculation

```
TIMER_ALARM_PERIOD = 16,000,000 Hz / 100 Hz = 160,000 SYSTIMER ticks
```

Each alarm fires every 10 ms → 100 Hz tick rate for ThreadX.

### Key Registers

| Register                  | Address           | Purpose                              |
|--------------------------|-------------------|--------------------------------------|
| SYSTIMER_CONF_REG        | BASE + 0x00       | Clock enable, comparator work enable |
| SYSTIMER_TARGET0_CONF_REG| BASE + 0x34       | Period value, mode, unit select      |
| SYSTIMER_COMP0_LOAD_REG  | BASE + 0x50       | Write-trigger: loads conf into hw    |
| SYSTIMER_INT_ENA_REG     | BASE + 0x64       | Enable interrupt per comparator      |
| SYSTIMER_INT_CLR_REG     | BASE + 0x6C       | Write-trigger: clear interrupt       |

**Important bits in SYSTIMER_CONF_REG**:
```
Bit  0: SYSTIMER_SYSTIMER_CLK_FO  — Force systimer clock on
Bit 24: SYSTIMER_TARGET0_WORK_EN  — Enable COMP0 to fire
Bit 30: SYSTIMER_TIMER_UNIT0_WORK_EN — Enable counter unit 0 (default=1)
```

**Important bits in SYSTIMER_TARGET0_CONF_REG**:
```
Bits [25:0]: Period value (26-bit, max ~67M ticks)
Bit     30:  PERIOD_MODE — 0=one-shot, 1=periodic (auto-reload)
Bit     31:  TIMER_UNIT_SEL — 0=use unit 0, 1=use unit 1
```

**Previous bug**: We set bit 31 in SYSTIMER_TARGET0_CONF_REG thinking it was
an "enable" bit. It is actually TIMER_UNIT_SEL. Setting it to 1 means COMP0
compares against unit 1 instead of unit 0, so it never fires (unit 0 keeps
counting but COMP0 doesn't see it).

**Previous bug**: We never set bit 24 (SYSTIMER_TARGET0_WORK_EN) in
SYSTIMER_CONF_REG. Without this bit, COMP0 is clocked but will not generate
an interrupt output.

### SYSTIMER Configuration Sequence

The correct sequence for periodic COMP0:

```c
// 1. Force clock on AND enable COMP0 work
write_reg(SYSTIMER_CONF_REG,
          read_reg(SYSTIMER_CONF_REG) | (1 << 0) | (1 << 24));

// 2. Disable COMP0 while reconfiguring (write 0 clears all fields)
write_reg(SYSTIMER_TARGET0_CONF_REG, 0);

// 3. Set period and enable periodic mode
//    bit 31 = 0 (unit 0), bit 30 = 1 (periodic), bits 25:0 = period
write_reg(SYSTIMER_TARGET0_CONF_REG,
          (160000 & 0x03FFFFFF) | BIT(30));

// 4. Trigger load into comparator hardware
write_reg(SYSTIMER_COMP0_LOAD_REG, 1);  // write-trigger, not sticky

// 5. Enable interrupt
write_reg(SYSTIMER_INT_ENA_REG, read_reg(SYSTIMER_INT_ENA_REG) | (1 << 0));

// 6. Clear any stale interrupt
write_reg(SYSTIMER_INT_CLR_REG, (1 << 0));
```

> **Update (Bug 27)**: The above is the original direct-register approach.
> Current code uses `systimer_hal_*` HAL functions instead (same as FreeRTOS),
> using counter 1 (`SYSTIMER_COUNTER_OS_TICK`) and alarm 0
> (`SYSTIMER_ALARM_OS_TICK_CORE0`). The HAL abstracts the register writes.

### ISR Cleanup Sequence

In the ISR, both the SYSTIMER and the PLIC edge latch must be cleared:

```c
// Clear SYSTIMER interrupt status (write-trigger)
write_reg(SYSTIMER_INT_CLR_REG, (1 << 0));

// Clear PLIC edge latch for CPU line 7
write_reg(PLIC_MX_CLEAR_REG, (1u << 7));

// Now call ThreadX tick handler
_tx_timer_interrupt();
```

If only SYSTIMER is cleared but not PLIC: the edge latch remains set and the
interrupt will fire again immediately after mret.

If only PLIC is cleared but not SYSTIMER: the SYSTIMER interrupt remains
asserted and will immediately re-trigger the PLIC.

> **Update (Bug 27)**: CPU line changed from 7 to 17. Current ISR uses HAL
> function `systimer_ll_clear_alarm_int()` and direct `PLIC_MX_CLEAR` write
> (not `esp_cpu_intr_edge_ack()` which has the same ROM masking issue as
> `esp_cpu_intr_enable()`).

---

## Update (Bug 27): Reserved and Disabled CPU Interrupt Lines

Not all 32 CPU interrupt lines are available for application use on ESP32-C6.
This was discovered when line 7 was found to silently ignore PLIC configuration.

### Hardware-Reserved Lines (CLINT-bound)

From `esp_hw_support/port/esp32c6/esp_cpu_intr.c`:
```c
const uint32_t rsvd_mask = BIT(1) | BIT(3) | BIT(4) | BIT(6) | BIT(7);
```

| Line | Reason |
|------|--------|
| 1    | Bound to Wi-Fi MAC (internal use) |
| 3    | CLINT (Core-Local Interrupt Timer) |
| 4    | CLINT |
| 6    | Permanently disabled |
| 7    | CLINT machine timer (`mtimecmp`/`mtime`) |

**Effect**: The ROM `esprv_intc_int_enable` function silently ignores any bit
in this mask. Writing the PLIC ENABLE bit directly (bypassing the ROM) also has
no effect — the hardware physically prevents these lines from being asserted by
the PLIC.

### Effectively-Disabled Lines (INTMTX routing)

From `esp_hw_support/intr_alloc.c`:
```
// Muxing an interrupt source to interrupt 6, 7, 11, 15, 16 or 29
// causes the interrupt to effectively be disabled.
```

These lines exist in the CPU but the INTMTX routing hardware ignores any source
routed to them. Even if you configure the PLIC for these lines, no peripheral
source will ever assert them.

### Safe Lines for Application Use

Lines not in either reserved list. FreeRTOS uses lines 2, 5, 8, 25, 27 (for
WDT, inter-core, etc.). We use **line 17** for SYSTIMER.

---

## Update (Bug 27): Additional mie CSR Requirements

The original Layer 3 section described two CSR bits (`mstatus.MIE` and
`mie.MEIE`). During Bug 27 investigation a third requirement was discovered.

### mie Bit N — Per-Line Enable (ESP32-C6 Non-Standard)

Standard RISC-V: `mie` has three named bits — MSIE (bit 3), MTIE (bit 7),
MEIE (bit 11). Bits above 11 are "platform-specific". Setting `mie.MEIE = 1`
is sufficient to receive all PLIC-enabled external interrupts.

**ESP32-C6 is different.** The PLIC hardware treats `mie bits [31:0]` as an
additional per-line enable mask. Bit N of `mie` acts as a second enable gate
for CPU interrupt line N, independent of `PLIC_MXINT_ENABLE_REG bit N`.

**Three conditions** must all be true for an external interrupt on line N:
1. `mstatus.MIE` (bit 3) = 1
2. `mie.MEIE` (bit 11) = 1
3. `mie bit N` = 1  ← **non-standard, ESP32-C6 specific**

**Evidence from diagnostic readbacks**:

```
FreeRTOS line 8 (WDT) — works:
  PLIC_ENABLE bit 8 = 1  (set by FreeRTOS PLIC config)
  mie bit 8         = 1  (also set by ROM esprv_intc_int_enable)

Our line 17 — initial attempt, broken:
  PLIC_ENABLE bit 17 = 1  (we set via direct register write)
  mie bit 17         = 0  (NOT set — direct write missed this)
  → isr_count stays 0 forever

Our line 17 — after fix, working:
  PLIC_ENABLE bit 17 = 1
  mie bit 17         = 1  (added csrs mie, (1u<<17))
  → isr_count increments
```

**Why direct PLIC writes missed this**: The ROM function `esprv_intc_int_enable`
does both steps internally — it writes `PLIC_MXINT_ENABLE_REG` AND does
`csrs mie, mask`. When we bypassed it with direct volatile writes, we only did
the first step. The `csrs mie` was silently missing.

**Fix**:
```c
PLIC_MX_ENABLE |= (1u << TIMER_CPU_INT_NUM);                          /* PLIC enable */
__asm__ volatile("csrs mie, %0" :: "r"(1u << TIMER_CPU_INT_NUM));     /* mie per-line bit */
```

### mideleg — Keep Interrupt in Machine Mode

`mideleg` controls whether each interrupt is delegated from M-mode to S-mode.
If `mideleg bit N = 1`, interrupt line N is handled in S-mode — M-mode handlers
are bypassed entirely. ThreadX runs in M-mode so bit 17 must be 0:

```c
__asm__ volatile("csrc mideleg, %0" :: "r"(1u << TIMER_CPU_INT_NUM));
```

`esp_intr_alloc()` does this same write (`RV_CLEAR_CSR(mideleg, BIT(intr))`)
for every interrupt it registers.

### Updated CSR Summary (after Bug 27)

| CSR      | Bit | Name       | How we set it                    | Purpose                                 |
|----------|-----|------------|----------------------------------|-----------------------------------------|
| mstatus  | 3   | MIE        | ThreadX (TX_DISABLE/RESTORE + mret) | Global machine interrupt gate        |
| mie      | 11  | MEIE       | `csrs mie, 0x800` at init        | All PLIC external interrupts enabled    |
| mie      | 17  | (per-line) | `csrs mie, (1u<<17)` at init     | ESP32-C6: per-line gate for CPU line 17 |
| mtvec    | —   | —          | `csrw mtvec, table_addr`         | 256-byte aligned vector table           |
| mideleg  | 17  | (per-line) | `csrc mideleg, (1u<<17)` at init | Keep line 17 in M-mode                  |

### The `csrs` / `csrc` / `csrw` / `csrr` Instructions

```asm
csrs csr, rs1   → CSR = CSR | rs1      (set bits — rs1 is a bitmask)
csrc csr, rs1   → CSR = CSR & ~rs1     (clear bits — rs1 is a bitmask)
csrw csr, rs1   → CSR = rs1            (write entire CSR)
csrr rd, csr    → rd = CSR             (read entire CSR)
```

All four are atomic read-modify-write (or read/write) operations. They cannot
be interrupted between the read and write phases.

---

## Update (Bug 28): isr_count=0 Diagnostic Run — All Registers Verified Correct

After applying Bug 27 fixes (line 17, direct PLIC writes, `csrs mie` per-line bit),
a hardware run produced the following diagnostic output. All checked registers
confirmed correct, yet `isr_count` remained 0.

### Diagnostic Register Readback (Bug 28 Run)

```
mtvec          = 0x4080af01
mie            = 0x0a020924
mideleg        = 0x00000011
INTMTX src57   = 0x00000011
PLIC ENABLE    = 0x0a020124
PLIC TYPE      = 0x00020000
PLIC PRI[7]    = 0x00000002   ← label wrong (reads line 17); value correct
PLIC THRESH    = 0x00000001
```

| Register | Value | Verification |
|---|---|---|
| mtvec top 24 bits | `0x4080af00` = `_tx_esp32c6_vector_table` | ✓ Vector table installed |
| mie bit 11 (MEIE) | `0x924 & 0x800 = 0x800` → set | ✓ All PLIC externals enabled |
| mie bit 17 | `0x0a020924 & 0x20000 = 0x20000` → set | ✓ Per-line enable set |
| mideleg bit 17 | `0x11 & 0x20000 = 0` → clear | ✓ M-mode handles line 17 |
| INTMTX[57] | `0x11` = 17 decimal | ✓ SYSTIMER_TARGET0 → CPU line 17 |
| PLIC ENABLE bit 17 | `0x0a020124 & 0x20000 = 0x20000` → set | ✓ PLIC line 17 enabled |
| PLIC TYPE bit 17 | `0x00020000 & 0x20000` → set | ✓ Edge-triggered |
| PLIC PRI[17] | `0x2` > THRESH `0x1` | ✓ Will not be masked by threshold |

**All configured registers are correct. The interrupt should fire. Root cause
lies in a value not yet measured.**

---

## Update (Bug 28): The `mstatus` CSR — Global Interrupt Gate

The diagnostic block prints `mie` (per-line enables) but does NOT print
`mstatus` (the global machine interrupt enable gate, `mstatus.MIE` = bit 3).

Both must be 1 simultaneously for any interrupt to reach the CPU.

### How ThreadX Manages `mstatus.MIE`

The scheduler (`tx_thread_schedule.S`, risc-v64/gnu port) does the following
before dispatching each thread via `mret`:

```asm
; _tx_thread_schedule: thread dispatch path (lines 203–208)
li    t0, 0x1880    ; MIE=0 (bit3), MPIE=1 (bit7), MPP=M-mode (bits12:11)
csrw  mstatus, t0   ; write full mstatus
mret                ; CPU: MIE ← MPIE (= 1), MPIE ← 1, privilege ← MPP
```

`0x1880` = `0b_0001_1000_1000_0000`:
- bit  3 = 0 (MIE — temporarily disabled at the `csrw` instant)
- bit  7 = 1 (MPIE — will become MIE after mret)
- bits 12:11 = `11` (MPP = machine mode)

`mret` atomically sets `MIE ← MPIE = 1`, so **threads execute with MIE = 1**.

### Spin-Loop Between Threads

The scheduler spin-loop (waiting for any thread to become ready) explicitly
enables MIE:

```asm
; _tx_thread_schedule: spin loop (line 72)
csrsi  mstatus, 0x08   ; set MIE = 1 (wait-for-interrupt with IRQs on)

; When a thread is found:
csrci  mstatus, 0x08   ; clear MIE = 0 (disable before loading thread context)
```

So during the spin loop MIE = 1, and during thread execution MIE = 1 (restored
by mret). In theory, SYSTIMER should be able to interrupt in both states.

### Root Cause Candidate A: Something Clears MIE After mret

If any code path between `mret` (thread dispatch) and the first SYSTIMER alarm
clears `mstatus.MIE`, the interrupt will never be taken. Candidates:
- ThreadX's `TX_DISABLE` macro at the start of some internal function
- An ESP-IDF function called from thread context that disables interrupts
- A compiler-generated memory barrier or fence that clears MIE (unlikely)

**Verification**: Read `mstatus` from inside the thread:

```c
uint32_t mstatus_val;
__asm__ volatile("csrr %0, mstatus" : "=r"(mstatus_val));
ESP_LOGI(TAG, "mstatus at thread start = 0x%08lx  (bit3 MIE must be 1)", mstatus_val);
```

---

## Update (Bug 28): SYSTIMER Counter Enable — Root Cause Candidate B

The diagnostic does NOT read SYSTIMER_CONF_REG (`0x60058000`). This register
controls whether counter 1 (SYSTIMER_COUNTER_OS_TICK) is actually running.

### SYSTIMER_CONF_REG Layout

| Bit | Name | Purpose |
|-----|------|---------|
| 31 | `timer_unit1_work_en` | Counter 1 (OS tick) running gate |
| 30 | `timer_unit0_work_en` | Counter 0 running gate |
| 24 | `target0_work_en` | Alarm 0 (OS tick alarm) enabled |
|  0 | `clk_fo` | Force systimer clock on |

### Concern

The HAL init sequence calls:
```c
systimer_ll_reset_register();       // writes CONF = 0 — clears ALL bits
...
systimer_hal_enable_counter(SYSTIMER_COUNTER_OS_TICK);  // should set bit 31
```

If `systimer_hal_enable_counter` for counter 1 does not correctly set bit 31
(e.g., due to a HAL bug, wrong counter index mapping, or a subtle ordering
issue), counter 1 stays frozen at 0. The alarm will never fire because the
counter never advances to the alarm threshold.

### Verification

```c
uint32_t systimer_conf = *(volatile uint32_t *)0x60058000;
ESP_LOGI("tx_diag", "SYSTIMER_CONF  = 0x%08lx  (bit31 ctr1_en, bit24 alm0_en)",
         systimer_conf);
```

If bit 31 = 0: counter 1 is stopped. Fix: call `systimer_hal_enable_counter`
again after `systimer_ll_reset_register`, or verify the HAL call uses the
correct counter index.

---

## Update (Bug 28): Secondary Bug in `_tx_thread_context_restore.S`

While auditing the upstream port assembly, a secondary bug was found.

**File**: `components/threadx/threadx/ports/risc-v64/gnu/src/tx_thread_context_restore.S`

**Symptom this will cause**: Crash on first successful ISR return (first time
`isr_count` would increment to 1). The CPU will jump to a garbage address.

### The Bug

In the `_tx_thread_no_preempt_restore` path (non-nested ISR return with no
thread switch), the code loads `mepc` from a hardcoded offset:

```asm
LOAD  t0, 240(sp)   ; hardcoded 240 bytes from current sp
csrw  mepc, t0
```

`240 = 30 * 8` (correct for RV64 where REGBYTES = 8).
`30 * REGBYTES = 30 * 4 = 120` on RV32 (ESP32-C6, REGBYTES = 4).

The `LOAD` macro correctly becomes `lw` on RV32 (from `tx_port.h`), but the
offset `240` is still the RV64 value. On RV32, `sp + 240` points 120 bytes
past where `mepc` was saved, reading whatever happens to be there (stack of a
different thread, or uninitialized data). The subsequent `mret` jumps to that
garbage address → instruction-fetch exception → crash.

### Why Not Yet Triggered

This path is only reached when:
1. A SYSTIMER ISR fires (enters `_tx_esp32c6_trap_handler`)
2. `_tx_thread_context_save` determines this is non-nested (no preemption needed)
3. Control returns through `_tx_thread_context_restore` → `_tx_thread_no_preempt_restore`

Since `isr_count` is currently 0 (ISR never fires), this path has not yet
been executed. The bug is dormant until the primary ISR-not-firing issue is fixed.

### Fix (Apply After Primary Issue Resolved)

In `_tx_thread_context_restore.S`, in `_tx_thread_no_preempt_restore`:

```asm
; Change from (RV64 hardcoded offset):
LOAD  t0, 240(sp)

; Change to (XLEN-agnostic using REGBYTES from tx_port.h):
LOAD  t0, 30*REGBYTES(sp)
```

On RV32: `30 * 4 = 120` → `lw t0, 120(sp)` — reads correct `mepc`.
On RV64: `30 * 8 = 240` → `ld t0, 240(sp)` — unchanged from current.

The `REGBYTES` macro is defined in `tx_port.h` via `__riscv_xlen` detection
and is available in assembly via the `#include "tx_port.h"` at the top of the
context restore file.

---

## Update (Bug 28): Proposed Diagnostic Additions

Three additions to `components/threadx/port/tx_esp32c6_timer.c` and
`main/main.c` to identify which root cause is active:

### In `_tx_port_setup_timer_interrupt` (timer.c diagnostic block)

Add immediately after the existing `csrr mie` readback:

```c
uint32_t mstatus_val;
__asm__ volatile("csrr %0, mstatus" : "=r"(mstatus_val));
ESP_LOGI("tx_diag", "mstatus        = 0x%08lx  (bit3 MIE, bit7 MPIE)", mstatus_val);
ESP_LOGI("tx_diag", "SYSTIMER_CONF  = 0x%08lx  (bit31 ctr1_en, bit30 ctr0_en, bit24 alm0_en)",
         *(volatile uint32_t *)0x60058000);
```

### In `blink_thread_entry` (main.c) — before the spin loop

```c
static void blink_thread_entry(ULONG param)
{
    (void)param;
    uint32_t ms_val;
    __asm__ volatile("csrr %0, mstatus" : "=r"(ms_val));
    ESP_LOGI(TAG, "[blink] mstatus at thread start = 0x%08lx (bit3 MIE must be 1)", ms_val);
    ULONG count = 0;
    while (1) { ... }
}
```

### Interpretation

| mstatus.MIE (thread) | SYSTIMER_CONF bit 31 | Root cause |
|---|---|---|
| 0 | — | **A**: Interrupts globally disabled in thread context |
| 1 | 0 | **B**: SYSTIMER counter 1 not running |
| 1 | 1 | Neither A nor B — investigate further (mtvec overwrite? wrong alarm?) |

---

## Level-Triggered vs Edge-Triggered for SYSTIMER (Bug 28 Root Cause)

### Why Edge-Triggered Failed

Previous design set `PLIC_MX_TYPE |= (1u << 17)` (edge-triggered). This was the root cause
of `isr_count=0`. With edge-triggered mode:

- PLIC only sets its latch when it sees a **rising edge (0→1)** on the input signal
- SYSTIMER INT_ST stays asserted (HIGH) permanently once the alarm fires unless the ISR clears it
- If there is no NEW rising edge after the PLIC is enabled and cleared, `mip.bit17` is never set
- The CPU never takes the interrupt

The diagnostic confirmed all registers correct — the only wrong register was
`PLIC TYPE = 0x00020000` (bit 17 = 1 = edge-triggered, should be 0 = level).

### Why Level-Triggered is Correct

FreeRTOS `port_systick.c vSystimerSetup()` uses `esp_intr_alloc()` **without** `ESP_INTR_FLAG_EDGE`,
which defaults to level-triggered. With level-triggered:

```
SYSTIMER alarm fires → INT_ST=1 → SYSTIMER output HIGH
PLIC sees HIGH level → mip.bit17=1 (continuously asserted while level HIGH)
CPU takes interrupt (MIE=1, mie.bit17=1)
ISR: systimer_ll_clear_alarm_int() → INT_ST=0 → SYSTIMER output LOW
PLIC sees LOW level → mip.bit17=0 (deasserted automatically)
mret → thread resumes with MIE=1
10ms later: next alarm → INT_ST=1 → cycle repeats
```

**No PLIC CLEAR write is needed in the ISR for level-triggered mode.** Only `systimer_ll_clear_alarm_int()`.

### PLIC EMIP (Edge Machine Interrupt Pending) Register

The `PLIC_EMIP_STATUS_REG` at `0x2000100C` (PLIC_MX_BASE + 0x0C) shows which
edge-triggered lines have their latch set. This is only meaningful for lines configured
as edge-triggered. For level-triggered operation this register is irrelevant.

Added to diagnostic readout for reference:
```c
ESP_LOGI("tx_diag", "PLIC EMIP = 0x%08lx  (edge-pending: bit17 only set if edge-mode)", PLIC_MX_EMIP);
```

### mip CSR — CPU-Visible Interrupt Pending

The `mip` CSR shows which interrupts are **currently pending from the CPU's perspective**.
This is the most direct indicator of whether the PLIC is delivering an interrupt:
- `mip.bit17 = 1` → CPU will take interrupt as soon as MIE=1
- `mip.bit17 = 0` → CPU does not see interrupt (PLIC not asserting, even if ENABLE set)

Added to diagnostic readout:
```c
ESP_LOGI("tx_diag", "mip = 0x%08lx  (bit17 pending = CPU sees interrupt)", mip_val);
```

### New Timer Configuration (level-triggered)

Setup:
```c
PLIC_MX_TYPE &= ~(1u << TIMER_CPU_INT_NUM);   /* bit 17 = 0 = level-triggered */
PLIC_MX_PRI_N = TIMER_CPU_INT_PRIORITY;       /* priority 2 > threshold 1 */
csrc mideleg, (1 << 17);                      /* not delegated to U-mode */
PLIC_MX_ENABLE |= (1u << TIMER_CPU_INT_NUM);  /* enable line 17 */
csrs mie, (1 << 17);                          /* enable mie bit 17 */
```

ISR:
```c
void _tx_esp32c6_timer_isr(void) {
    g_tx_timer_isr_count++;
    systimer_ll_clear_alarm_int(s_systimer_hal.dev, SYSTIMER_ALARM_OS_TICK_CORE0);
    /* NO PLIC_MX_CLEAR — not needed for level-triggered */
    _tx_timer_interrupt();
}
```


---

## Our Assembly Approach vs esp_intr_alloc (Architectural Comparison)

This section documents why we use direct register writes + our own mtvec vector table
instead of the standard ESP-IDF `esp_intr_alloc()` API, and exactly what differs.

### What esp_intr_alloc() Does (FreeRTOS path)

```
esp_intr_alloc(ETS_SYSTIMER_TARGET0_INTR_SOURCE, ESP_INTR_FLAG_IRAM | level, handler, arg, NULL)
```

Internally, `esp_intr_alloc()` does all of these:

1. **Finds a free CPU interrupt line** (picks from available lines, e.g. 2, 5, 8, 25, 27 for FreeRTOS)
2. **Routes peripheral → CPU line** via INTMTX: `REG_WRITE(DR_REG_INTERRUPT_MATRIX_BASE + 4*src, cpu_int)`
3. **Sets priority**: `esprv_int_set_priority(cpu_int, level)` → ROM function → writes PLIC PRI[N]
4. **Sets trigger type**: `esprv_int_set_type(cpu_int, INTR_TYPE_LEVEL)` → ROM function → clears PLIC TYPE bit N
5. **Clears mideleg bit**: `RV_CLEAR_CSR(mideleg, BIT(cpu_int))` → keeps interrupt in machine mode
6. **Registers the C handler** in the ESP-IDF software dispatch table `_interrupt_table[N]`
7. **Enables the line**: `esp_cpu_intr_enable(1 << cpu_int)` → calls ROM `esprv_intc_int_enable(mask)`
   which does BOTH: `PLIC_ENABLE |= mask` AND `csrs mie, mask`
8. **mtvec still points to `_vector_table`** (the ESP-IDF default vector table in `vectors_intc.S`)

When the interrupt fires:
```
CPU → vector[N] → j _interrupt_handler (common handler)
  → _interrupt_handler: reads mcause, looks up _interrupt_table[N], calls registered C function
  → C handler: clears SYSTIMER INT_ST, calls xPortSysTickHandler()
  → saves/restores context using FreeRTOS port assembly
```

### Our Approach (ThreadX direct control)

We bypass `esp_intr_alloc()` entirely because:
- `esp_intr_alloc()` uses `portENTER_CRITICAL(&spinlock)` internally — this calls FreeRTOS
  critical section code which conflicts with ThreadX (the ThreadX FreeRTOS compat layer
  has not yet been fully initialized when the timer is configured)
- We need full control of the interrupt entry/exit path for ThreadX context save/restore
- We replace mtvec with our own vector table that jumps directly to our handler

What we do instead:

1. **INTMTX routing**: `esp_rom_route_intr_matrix(0, ETS_SYSTIMER_TARGET0_INTR_SOURCE, 17)` (ROM call, safe)
2. **SYSTIMER HAL**: standard `systimer_hal_*` calls (no FreeRTOS dependencies)
3. **PLIC direct writes**: volatile pointer writes to 0x20001000 registers (bypasses ROM esprv_* functions)
4. **mideleg CSR**: `csrc mideleg, (1<<17)` inline asm
5. **mie CSR**: `csrs mie, (1<<17)` inline asm
6. **mtvec**: overwritten in `_tx_initialize_low_level` to point to `_tx_esp32c6_vector_table`
   (our 256-byte aligned table with dedicated entry at vector[17])

When the interrupt fires:
```
CPU → _tx_esp32c6_vector_table[17] → j _tx_esp32c6_trap_handler (direct, no dispatch table)
  → addi sp, sp, -(32*REGBYTES)
  → STORE x1, 28*REGBYTES(sp)
  → call _tx_thread_context_save     ← ThreadX context save
  → call _tx_esp32c6_timer_isr       ← clears SYSTIMER, calls _tx_timer_interrupt()
  → la t0, _tx_thread_context_restore
  → jr t0                            ← ThreadX context restore + mret
```

### Key Differences Summary

| Aspect | esp_intr_alloc (FreeRTOS) | Our approach (ThreadX) |
|--------|--------------------------|------------------------|
| Interrupt routing | `esp_intr_alloc()` API | Direct: ROM + volatile writes |
| PLIC configuration | ROM `esprv_intc_int_*` functions | Direct volatile register writes |
| mie CSR | ROM `esprv_intc_int_enable()` does csrs | Explicit `csrs mie, mask` inline asm |
| mtvec | ESP-IDF `_vector_table` → `_interrupt_handler` | Our `_tx_esp32c6_vector_table` |
| Interrupt dispatch | Software table lookup (`_interrupt_table[N]`) | Direct jump from vector table entry |
| Context save/restore | FreeRTOS port assembly | ThreadX `_tx_thread_context_save/restore` |
| Trigger type | Level (default, no ESP_INTR_FLAG_EDGE) | Level (TYPE bit N = 0) — **must match\!** |
| ISR acknowledgment | Only `systimer_ll_clear_alarm_int()` | Only `systimer_ll_clear_alarm_int()` |

### The Critical Trigger Type Lesson

The single most important difference that caused Bug 28:

- `esp_intr_alloc` → `esprv_int_set_type(N, INTR_TYPE_LEVEL)` → clears PLIC TYPE bit N
- Our old code → `PLIC_MX_TYPE |= (1u << N)` → **set** PLIC TYPE bit N = EDGE

FreeRTOS always uses **level-triggered** for SYSTIMER. We mistakenly used edge-triggered.
Level-triggered works with SYSTIMER because INT_ST is a stable level signal (stays HIGH
until explicitly cleared). Edge detection is fragile — if the signal is already HIGH when
you configure the PLIC, no rising edge occurs and the interrupt is silently lost.

**Rule**: For any peripheral that uses a level output (INT_ST style), use level-triggered in the PLIC.
Only use edge-triggered for peripherals that give clean single-cycle pulses.


---

## Interrupt Initialization Timing — Disable MIE Before Timer Setup (Bug 29)

### The Problem: Level-Triggered + Early Timer Fire = Permanent Hang

After fixing Bug 28 (switching to level-triggered PLIC), a new failure mode appeared:
the system hangs completely after startup with **no thread output at all**.

Root cause: ESP-IDF leaves `mstatus.MIE=1` (global interrupt enable) during startup.
With level-triggered PLIC:

1. `_tx_initialize_low_level()` arms SYSTIMER (~10ms to first tick)
2. `mstatus.MIE=1` → CPU will take any pending interrupt immediately
3. SYSTIMER alarm fires → PLIC asserts mip.bit17 → CPU takes interrupt
4. **`tx_application_define()` has not run yet — no threads exist**
5. `_tx_thread_context_restore` finds `_tx_thread_execute_ptr == NULL`
6. Enters idle-wait path → **hangs forever**
7. `tx_application_define()` never executes → no threads ever created

With **edge-triggered** PLIC (Bug 28's wrong setting), the ISR never fired at all,
so this particular hang didn't occur — it just silently skipped every interrupt.
With **level-triggered** PLIC (correct), the ISR fires reliably — but fires too early
if `mstatus.MIE` is not cleared first.

### Fix: Clear mstatus.MIE at the Start of _tx_initialize_low_level

```asm
_tx_initialize_low_level:
    csrci   mstatus, 0x8    /* clear MIE (bit 3) — prevent premature timer ISR */
    ...
    /* setup timer, set mie.MEIE (bit 11) ... */
    ret

tx_application_define():
    /* create threads safely — no interrupts can fire (mstatus.MIE=0) */

_tx_thread_schedule:
    mret                    /* MPIE→MIE=1 — NOW interrupts are enabled */
```

### The mstatus.MIE vs mie.MEIE Distinction

These are two separate interrupt gates:

| CSR | Bit | Role |
|-----|-----|------|
| `mstatus.MIE` | bit 3 | **Global** interrupt enable — CPU-wide master switch |
| `mie.MEIE` | bit 11 | Per-type enable for Machine External Interrupts (PLIC channel) |

Setting `mie.MEIE=1` while `mstatus.MIE=0` is safe:
- The interrupt will be **pending** in `mip.bit11` (PLIC maintains its state)
- The CPU will **not take** the interrupt (mstatus.MIE=0 blocks delivery)
- When `mret` restores `mstatus.MIE=1` (via MPIE), any pending interrupt fires immediately

This is exactly what we want: arm the timer, run `tx_application_define()` safely,
then let the scheduler enable interrupts at the right moment.

### RISC-V mret and mstatus.MPIE

`mret` (Machine Return) in RISC-V:
1. Copies `mstatus.MPIE` → `mstatus.MIE` (restoring the saved interrupt state)
2. Sets `mstatus.MPIE = 1` (ready for next interrupt)
3. Jumps to `mepc`

When ThreadX sets up the first thread to run (`_tx_thread_schedule`), it:
- Sets `mepc` to the thread's entry point
- Sets `mstatus.MPIE = 1` in the saved context (so `mret` enables interrupts)
- Executes `mret`

Result: first thread runs with MIE=1 — timer interrupts now fire at the correct time.

### Rule for RTOS Integration with ESP-IDF

When integrating any RTOS into ESP-IDF (which leaves MIE=1 during startup):

1. **Always** `csrci mstatus, 0x8` at the start of the RTOS low-level init function
2. Let the RTOS scheduler re-enable MIE via its first `mret` 
3. Do not rely on the calling code to have left interrupts disabled

This is standard practice for bare-metal RISC-V RTOS ports. The ThreadX
`risc-v32/gnu` port's upstream `tx_initialize_low_level.S` examples typically
assume interrupts are already disabled — on ESP32-C6 with ESP-IDF they are not,
so we must explicitly disable them.

