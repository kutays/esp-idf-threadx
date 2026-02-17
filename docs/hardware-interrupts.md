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
