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
      Configured via: esp_rom_route_intr_matrix()
         │
         ▼
  [2] PLIC Machine-mode (PLIC MX)  0x20001000
      Controls enable/priority/type per CPU line
      Configured via: esp_cpu_intr_set_type/priority/enable()
         │
         ▼
  [3] RISC-V mie CSR + mstatus.MIE
      CPU-level enable gates
      mie.MEIE set by our assembly init, mstatus.MIE managed by ThreadX
         │
         ▼
      CPU core
```

Each layer must be properly configured. Missing any one layer = no interrupt.

---

## How We Configure Each Layer

Unlike the initial implementation (which used raw register writes that had
multiple bugs), the current code uses the same ESP-IDF APIs that FreeRTOS uses.

### Layer 1: INTMTX — `esp_rom_route_intr_matrix()`

```c
// Route SYSTIMER_TARGET0 (source 57) to CPU interrupt line 7
esp_rom_route_intr_matrix(0, ETS_SYSTIMER_TARGET0_INTR_SOURCE, TIMER_CPU_INT_NUM);
```

This is a **ROM function** — it is burned into the ESP32-C6 ROM and always
correct. No manual address calculation needed.

**Source number**: `ETS_SYSTIMER_TARGET0_INTR_SOURCE = 57`
Defined in `soc/esp32c6/include/soc/interrupts.h` as an enum value.

### Layer 2: PLIC — `esp_cpu_intr_*()` from esp_hw_support

```c
esp_cpu_intr_set_type(TIMER_CPU_INT_NUM, ESP_CPU_INTR_TYPE_EDGE);
esp_cpu_intr_set_priority(TIMER_CPU_INT_NUM, 1);
esp_cpu_intr_edge_ack(TIMER_CPU_INT_NUM);     // clear stale latch
esp_cpu_intr_enable(1u << TIMER_CPU_INT_NUM);
```

These are inline functions from `esp_cpu.h` that write the correct PLIC MX
registers at 0x20001000. No manual register address calculation.

### Layer 3: mie.MEIE — assembly (cannot be avoided)

```asm
li    t0, 0x800      # bit 11 = MEIE
csrs  mie, t0        # set MEIE in mie CSR
```

This is one instruction. There is no C API to set arbitrary CSR bits —
CSR access requires assembly or compiler intrinsics. This single instruction
is what enables the PLIC to deliver interrupts to the CPU core.

---

## Why the Timer Code Has NO Assembly

The user (correctly) observed that FreeRTOS on ESP32-C6 has no timer-related
assembly at all. Looking at `components/freertos/port_systick.c`:

```c
// FreeRTOS does this entirely in C:
void vSystimerSetup(void) {
    esp_intr_alloc(ETS_SYSTIMER_TARGET0_INTR_SOURCE, flags, SysTickIsrHandler, ...);
    systimer_hal_init(&systimer_hal);
    systimer_hal_set_alarm_period(&systimer_hal, alarm_id, 1000000UL / HZ);
    systimer_hal_select_alarm_mode(..., SYSTIMER_ALARM_MODE_PERIOD);
    systimer_hal_enable_alarm_int(...);
    systimer_hal_enable_counter(...);
}
```

We follow the exact same pattern. Our `_tx_port_setup_timer_interrupt()` and
`_tx_esp32c6_timer_isr()` are both pure C using the same HAL.

The assembly that DOES remain in our codebase is **not for the timer** — it is
for the ThreadX **context switch mechanism**:

| Assembly | Reason | Can it be C? |
|----------|--------|--------------|
| `csrw mtvec, t0` | Install trap vector | No — CSR write requires asm |
| `csrs mie, 0x800` | Enable mie.MEIE | No — CSR write requires asm |
| `_tx_esp32c6_trap_handler` | Save/restore registers on interrupt | No — must manipulate `sp` at a level C can't |
| Context save/restore (upstream) | Full thread register save/restore | No — RISC-V assembly required |

The trap handler IS assembly, but it has nothing to do with the timer — it is
the generic interrupt entry point for ALL interrupts, just like FreeRTOS's own
`portasm.S` which FreeRTOS also writes in assembly.

---

## SYSTIMER Configuration

**Reference**: `components/freertos/port_systick.c` for FreeRTOS implementation
**Our implementation**: `components/threadx/port/tx_timer_interrupt.c`

The SYSTIMER has:
- 2 free-running 52-bit counter units
- 3 comparators/alarms

We use **counter 1** (`SYSTIMER_COUNTER_OS_TICK`) and **alarm 0**
(`SYSTIMER_ALARM_OS_TICK_CORE0`), the same as FreeRTOS. Counter 0 is used
by `esp_timer` for the high-resolution timer, so we leave it alone.

**Tick calculation via HAL**: The `systimer_hal_set_alarm_period()` function
takes **microseconds** and uses the `us_to_ticks` callback to convert.
For 100 Hz: `period = 1,000,000 / 100 = 10,000 µs`.

The conversion functions `systimer_ticks_to_us` / `systimer_us_to_ticks` are
provided by `esp_private/systimer.h` (from `esp_hw_support`). They know the
SYSTIMER clock frequency (16 MHz on C6) and handle the conversion correctly.

### Setup Sequence

```c
// 1. Enable peripheral bus clock
PERIPH_RCC_ACQUIRE_ATOMIC(PERIPH_SYSTIMER_MODULE, ref_count) {
    if (ref_count == 0) {
        systimer_ll_enable_bus_clock(true);
        systimer_ll_reset_register();
    }
}

// 2. Init HAL context
systimer_hal_init(&s_systimer_hal);

// 3. Provide µs conversion callbacks
systimer_hal_set_tick_rate_ops(&s_systimer_hal, &ops);  // ops = { ticks_to_us, us_to_ticks }

// 4. Reset counter
systimer_ll_set_counter_value(s_systimer_hal.dev, SYSTIMER_COUNTER_OS_TICK, 0);
systimer_ll_apply_counter_value(s_systimer_hal.dev, SYSTIMER_COUNTER_OS_TICK);

// 5. Connect alarm to counter
systimer_hal_connect_alarm_counter(&s_systimer_hal, SYSTIMER_ALARM_OS_TICK_CORE0, SYSTIMER_COUNTER_OS_TICK);

// 6. Set period (µs) and mode
systimer_hal_set_alarm_period(&s_systimer_hal, SYSTIMER_ALARM_OS_TICK_CORE0, 10000);  // 10ms
systimer_hal_select_alarm_mode(&s_systimer_hal, SYSTIMER_ALARM_OS_TICK_CORE0, SYSTIMER_ALARM_MODE_PERIOD);

// 7. Enable interrupt and start counter
systimer_hal_enable_alarm_int(&s_systimer_hal, SYSTIMER_ALARM_OS_TICK_CORE0);
systimer_hal_enable_counter(&s_systimer_hal, SYSTIMER_COUNTER_OS_TICK);
```

### ISR Cleanup Sequence

```c
// 1. Clear the SYSTIMER alarm interrupt
systimer_ll_clear_alarm_int(s_systimer_hal.dev, SYSTIMER_ALARM_OS_TICK_CORE0);

// 2. Acknowledge the PLIC edge latch
esp_cpu_intr_edge_ack(TIMER_CPU_INT_NUM);

// 3. Advance ThreadX tick
_tx_timer_interrupt();
```

---

## Previous Bugs (All Fixed by Using HAL)

The raw-register-write approach had these bugs, all of which are avoided by
using the HAL:

| Bug | Raw Register Approach | HAL Approach |
|-----|-----------------------|--------------|
| Wrong source number (37→57) | Hardcoded, got it wrong | `ETS_SYSTIMER_TARGET0_INTR_SOURCE` enum |
| Wrong INTMTX formula (+0x80 offset) | Guessed wrong | `esp_rom_route_intr_matrix()` ROM function |
| Wrong controller (INTPRI vs PLIC MX) | Used 0x600C5000 not 0x20001000 | `esp_cpu_intr_*` always uses correct one |
| SYSTIMER_TARGET0_WORK_EN missing | Forgot bit 24 of CONF_REG | `systimer_hal_enable_counter()` sets this |
| TIMER_UNIT_SEL wrongly set (bit 31) | Set it thinking it was enable | `systimer_hal_connect_alarm_counter()` handles this |
| mie.MEIE never set | Not known to be needed | Still requires 1 asm instruction (unavoidable) |
