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
  Write value = 17  (CPU interrupt line 17)
```

**Reference**: `soc/esp32c6/include/soc/interrupts.h`
```c
// Counted from the enum in that file:
ETS_SYSTIMER_TARGET0_INTR_SOURCE = 57
```

**Important**: The old code had `base + 0x80 + src*4` which is wrong.
The correct formula is simply `base + src*4`.

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

Interrupt fires if: `priority[N] > threshold` AND `enable[N] = 1` AND `mie bit N = 1`

**Threshold note**: FreeRTOS startup sets `PLIC_MXINT_THRESH_REG = 1`
(constant `RVHAL_INTR_ENABLE_THRESH` in `hal/rv_utils.h`). This means any
interrupt with priority ≤ 1 is **masked**. Our SYSTIMER uses priority 2 to be
strictly above this threshold.

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

## Reserved and Disabled CPU Interrupt Lines

Not all 32 CPU interrupt lines are available for application use on ESP32-C6.

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
Muxing an interrupt source to interrupt 6, 7, 11, 15, 16 or 29
causes the interrupt to effectively be disabled.
```

These lines exist in the CPU but the INTMTX routing hardware ignores any source
routed to them. Even if you configure the PLIC for these lines, no peripheral
source will ever assert them.

### Safe Lines for Application Use

Lines not in either list above. FreeRTOS uses lines 2, 5, 8, 25, 27 (for WDT,
inter-core, etc.). We use **line 17** for SYSTIMER.

---

## Layer 3: RISC-V CSR Gates

**Three** CSR conditions must all be true for any external interrupt to reach
the CPU core on ESP32-C6. Missing any one of them silently prevents the interrupt.

### mstatus.MIE (bit 3) — Global Gate

Global machine interrupt enable. When 0, all interrupts are blocked regardless
of PLIC or `mie` configuration.

ThreadX manages this automatically:
- `TX_DISABLE` (`csrci mstatus, 8`) clears it during critical sections
- `TX_RESTORE` restores the saved value
- `mret` at the end of an ISR restores `mstatus.MIE` from the value saved in
  `mstatus.MPIE` at interrupt entry (hardware does this automatically)

### mie.MEIE (bit 11) — External Interrupt Gate

Machine External Interrupt Enable. This is the gate for the entire PLIC
external interrupt subsystem. When 0, no PLIC interrupt (regardless of priority,
enable bits, or `mie` per-line bits) reaches the CPU.

Standard RISC-V designs: setting `mie.MEIE = 1` is sufficient to receive all
PLIC-enabled external interrupts.

We set this in `_tx_initialize_low_level`:
```asm
li    t0, 0x800      /* bit 11 = MEIE */
csrs  mie, t0        /* atomically OR: mie |= 0x800 */
```

### mie Bit N — Per-Line Enable (ESP32-C6 Non-Standard)

This is the **non-standard, undocumented** requirement specific to the ESP32-C6
PLIC implementation. In addition to `mie.MEIE` (bit 11), the ESP32-C6 PLIC also
requires `mie bit N` to be set for CPU interrupt line N.

Standard RISC-V: `mie` has named bits for software interrupt (bit 3 MSIE),
timer interrupt (bit 7 MTIE), and external interrupt (bit 11 MEIE). Bits above
11 are defined as "platform-specific" by the spec.

ESP32-C6: The PLIC hardware treats `mie bits [31:0]` as an additional per-line
enable mask. Bit N in `mie` acts as a second enable gate for CPU interrupt line N,
independent of `PLIC_MXINT_ENABLE_REG bit N`.

**Evidence**: Comparing FreeRTOS working interrupt lines (2, 5, 8, 25, 27) vs
our line 17, with diagnostic register readbacks:

```
FreeRTOS line 8 (WDT):
  PLIC_ENABLE bit 8 = 1  ← set by PLIC
  mie bit 8       = 1  ← also set
  → fires correctly

Our line 17 (initial attempt):
  PLIC_ENABLE bit 17 = 1  ← we set this
  mie bit 17        = 0  ← NOT set
  → never fires

Our line 17 (after fix):
  PLIC_ENABLE bit 17 = 1  ← we set this
  mie bit 17        = 1  ← we also set this
  → fires correctly
```

**Root cause of missing bit**: The ROM function `esprv_intc_int_enable` does
both writes internally:
```c
// Inferred from behavior and ESP-IDF source reading:
PLIC_MXINT_ENABLE_REG |= mask;   // write PLIC register
csrs mie, mask;                  // also set mie bits
```

When we bypassed the ROM function with direct volatile register writes to fix
the reserved-line issue (Bug 27 Part 1), we wrote `PLIC_MXINT_ENABLE_REG` but
forgot the `csrs mie` step. The PLIC enable was set; the CPU gate was not.

**Fix**: After writing `PLIC_MXINT_ENABLE_REG`, also explicitly set the per-line
`mie` bit:
```c
PLIC_MX_ENABLE |= (1u << TIMER_CPU_INT_NUM);             /* PLIC enable */
__asm__ volatile("csrs mie, %0" :: "r"(1u << TIMER_CPU_INT_NUM));  /* mie bit */
```

### mideleg — Interrupt Mode Delegation

`mideleg` is a CSR controlling whether each interrupt is handled in M-mode
(machine mode) or delegated to S-mode (supervisor mode). If `mideleg bit N = 1`,
CPU interrupt line N is handled entirely in S-mode — M-mode handlers are bypassed.

ThreadX runs in M-mode. We must keep `mideleg bit 17 = 0`:
```c
__asm__ volatile("csrc mideleg, %0" :: "r"(1u << TIMER_CPU_INT_NUM));
```

`esp_intr_alloc()` does this same write (`RV_CLEAR_CSR(mideleg, BIT(intr))`)
for every interrupt it allocates via the ESP-IDF system.

### Complete CSR Picture for Line 17 (SYSTIMER)

```
mstatus.MIE (bit 3):   managed by ThreadX via TX_DISABLE/TX_RESTORE + mret
mie.MEIE    (bit 11):  set at init: csrs mie, 0x800
mie bit 17  (bit 17):  set at init: csrs mie, (1u << 17)  ← ESP32-C6 non-standard
mideleg bit 17:        cleared at init: csrc mideleg, (1u << 17)
```

### Summary of All CSR Bits We Touch

| CSR      | Bit | Name        | How we set it          | Purpose                                         |
|----------|-----|-------------|------------------------|-------------------------------------------------|
| mstatus  | 3   | MIE         | ThreadX (TX_DISABLE/RESTORE + mret) | Global machine interrupt gate    |
| mie      | 11  | MEIE        | `csrs mie, 0x800` at init  | All PLIC external interrupts enabled        |
| mie      | 17  | (per-line)  | `csrs mie, (1u<<17)` at init | ESP32-C6: per-line gate for CPU line 17 |
| mtvec    | —   | —           | `csrw mtvec, table_addr`   | Points to 256-byte aligned vector table    |
| mideleg  | 17  | (per-line)  | `csrc mideleg, (1u<<17)` at init | Keep line 17 in M-mode         |

### The `csrs` / `csrc` Instruction

```asm
csrs csr, rs1   → CSR = CSR | rs1    (set bits — rs1 is a bitmask)
csrc csr, rs1   → CSR = CSR & ~rs1   (clear bits — rs1 is a bitmask)
csrw csr, rs1   → CSR = rs1          (write entire CSR value)
csrr rd, csr    → rd = CSR           (read entire CSR value)
```

These are atomic read-modify-write operations. They cannot be interrupted
between the read and write phases.

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

### ISR Cleanup Sequence

In the ISR, both the SYSTIMER and the PLIC edge latch must be cleared:

```c
// Clear SYSTIMER alarm interrupt (stops the peripheral asserting the line)
systimer_ll_clear_alarm_int(hal->dev, SYSTIMER_ALARM_OS_TICK_CORE0);

// Clear PLIC edge latch for CPU line 17
// Required for edge-triggered mode: without this the interrupt re-fires
// immediately after mret because the latch is still asserted.
PLIC_MX_CLEAR |= (1u << TIMER_CPU_INT_NUM);   // TIMER_CPU_INT_NUM = 17

// Advance the ThreadX tick counter
_tx_timer_interrupt();
```

If only SYSTIMER is cleared but not PLIC: the edge latch remains set and the
interrupt fires again immediately after `mret`.

If only PLIC is cleared but not SYSTIMER: the SYSTIMER interrupt remains
asserted and will immediately re-trigger the PLIC edge latch.

**Note**: We previously called `esp_cpu_intr_edge_ack()` for PLIC edge clear,
but that proxies to the same ROM `esprv_intc_int_clear` function which
also has the reserved-line masking issue. Direct register write is used instead.
