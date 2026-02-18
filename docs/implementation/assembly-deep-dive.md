# Assembly Deep Dive

This document explains every assembly instruction in the two assembly files:
- `components/threadx/port/tx_initialize_low_level.S` (our file)
- The ThreadX risc-v64/gnu port files (upstream, used unchanged)

---

## RISC-V Primer for This Port

### Register Conventions (RISC-V ABI)

| Register | ABI Name | Role                           | Caller/Callee saved |
|----------|----------|--------------------------------|---------------------|
| x0       | zero     | Always zero (hardwired)        | —                   |
| x1       | ra       | Return address                 | Caller              |
| x2       | sp       | Stack pointer                  | Callee              |
| x3       | gp       | Global pointer                 | —                   |
| x4       | tp       | Thread pointer                 | —                   |
| x5–x7   | t0–t2   | Temporaries                    | Caller              |
| x8       | s0/fp    | Saved / frame pointer          | Callee              |
| x9       | s1       | Saved register                 | Callee              |
| x10–x11 | a0–a1   | Function arguments / return    | Caller              |
| x12–x17 | a2–a7   | Function arguments             | Caller              |
| x18–x27 | s2–s11  | Saved registers                | Callee              |
| x28–x31 | t3–t6   | Temporaries                    | Caller              |

### STORE/LOAD/REGBYTES Macros

The ThreadX risc-v64/gnu port uses these macros defined in `tx_port.h`:

```c
// From ports/risc-v64/gnu/inc/tx_port.h
#if __riscv_xlen == 64
    #define STORE   sd          // store doubleword (8 bytes)
    #define LOAD    ld          // load doubleword (8 bytes)
    #define REGBYTES 8
#else
    #define STORE   sw          // store word (4 bytes)
    #define LOAD    lw          // load word (4 bytes)
    #define REGBYTES 4
#endif
```

On ESP32-C6 (RV32), the compiler defines `__riscv_xlen = 32`, so:
- `STORE` → `sw` (32-bit store)
- `LOAD` → `lw` (32-bit load)
- `REGBYTES` → `4`

This is why we can use the risc-v64 port on a 32-bit CPU — it adapts at compile time.

### Key CSR Instructions

```asm
csrw  csr, rs1       # CSR = rs1 (write)
csrr  rd, csr        # rd = CSR (read)
csrs  csr, rs1       # CSR = CSR | rs1 (set bits)
csrc  csr, rs1       # CSR = CSR & ~rs1 (clear bits)
csrsi csr, uimm5     # CSR = CSR | uimm5 (set bits, immediate)
csrci csr, uimm5     # CSR = CSR & ~uimm5 (clear bits, immediate)
```

### RISC-V Machine Mode Trap Entry

When an interrupt or exception fires in machine mode:
1. **mepc** ← PC of the instruction that was interrupted
2. **mcause** ← reason (bit 31 = 1 for interrupt, bits 30:0 = interrupt/exception code)
3. **mstatus.MPIE** ← old mstatus.MIE (saved)
4. **mstatus.MIE** ← 0 (interrupts disabled atomically)
5. **mstatus.MPP** ← previous privilege (11 for machine mode)
6. PC ← mtvec (in direct mode, bit 1:0 = 00)

**mret** instruction reverses this:
1. PC ← mepc
2. mstatus.MIE ← mstatus.MPIE (restored)
3. mstatus.MPIE ← 1

---

## tx_initialize_low_level.S — Full Annotation

Source: `components/threadx/port/tx_initialize_low_level.S`

```asm
#include "tx_port.h"
```
Brings in `STORE`, `LOAD`, `REGBYTES` macros from the risc-v64 port header.

```asm
    .section .text
    .global _tx_initialize_low_level
    .global _tx_thread_context_save
    .global _tx_thread_context_restore
```
Places code in the `.text` section. `global` makes symbols visible to the linker.
`_tx_thread_context_save` and `_tx_thread_context_restore` are defined in the
upstream port assembly files — they are referenced here for documentation.

```asm
    .extern _tx_initialize_unused_memory
    .extern _tx_thread_system_stack_ptr
    .extern _tx_port_setup_timer_interrupt
    .extern _tx_esp32c6_timer_isr
    .extern _tx_esp32c6_timer_cpu_int
```
Declares external symbols. `.extern` in GNU as is informational — the linker
resolves these at link time regardless of whether `.extern` is present.

```asm
    .equ PLIC_MX_EIP_STATUS_REG, 0x2000100C
```
`.equ` defines an assembler constant (like `#define` in C). This is the PLIC
machine-mode EIP status register address — read it to find which CPU interrupt
line is pending.

```asm
    .equ INT_FRAME_SIZE, (32 * REGBYTES)
```
On RV32: `32 * 4 = 128 bytes`. This is the interrupt stack frame we allocate
to hold all 32 general-purpose registers. The ThreadX context save code uses
offsets 12*REGBYTES through 31*REGBYTES within this frame (see context save
analysis below).

---

### _tx_initialize_low_level

```asm
_tx_initialize_low_level:
    addi    sp, sp, -16
    STORE   ra, 0(sp)
```
Standard function prologue. We need to call `_tx_port_setup_timer_interrupt`
(a C function, which uses `ra` as its return address), so we save our own `ra`
on the stack first. `addi sp, sp, -16` grows the stack downward by 16 bytes
(keeps 16-byte alignment required by the RISC-V calling convention).

`STORE ra, 0(sp)` → `sw ra, 0(sp)` on RV32: stores the 4-byte return address
at the top of our frame.

```asm
    la      t0, _tx_thread_system_stack_ptr
    STORE   sp, 0(t0)
```
`la` (load address) is a pseudo-instruction that expands to an `auipc` + `addi`
sequence to load a symbol's address into a register.

`_tx_thread_system_stack_ptr` is a global variable in the ThreadX kernel. ISRs
switch to this stack when they detect a first-level (non-nested) interrupt.

We store the current `sp` into that global. At this point, `sp` points to the
ESP-IDF startup stack, which is large and reliable. ThreadX ISRs will run on
this stack.

Note: We save `sp` **after** the prologue (`addi sp, sp, -16`), so the stored
value points into our own frame. This is intentional — the system stack starts
below our frame, ensuring the ISR stack doesn't collide with our local data.

```asm
    la      t0, _tx_initialize_unused_memory
    la      t1, __tx_free_memory_start
    STORE   t1, 0(t0)
```
`_tx_initialize_unused_memory` is another ThreadX global: a pointer to the
first byte of memory ThreadX hasn't yet allocated.

`__tx_free_memory_start` is a symbol we define in the `.data` section at the
end of this file (a 4-byte placeholder). Its address marks the beginning of
memory ThreadX considers "free" for its own use.

We write the address of our free memory marker into the ThreadX global.

```asm
    la      t0, _tx_esp32c6_trap_handler
    csrw    mtvec, t0
```
`csrw mtvec, t0` writes our trap handler's address into the Machine Trap-Vector
Base-Address CSR.

The low 2 bits of `mtvec` select the mode:
- `00` = **Direct mode**: all traps jump to `mtvec`
- `01` = **Vectored mode**: interrupts jump to `mtvec + 4 * cause`

Since our handler address is 4-byte aligned (guaranteed by `.align 4` on the
handler), bit 0 is 0, so direct mode is selected automatically.

In direct mode, every exception AND every interrupt arrives at the same handler.
We inspect `mcause` to distinguish them.

**WARNING for audit**: ESP-IDF's startup code also sets `mtvec` to its own
exception handler before calling `esp_startup_start_app`. We overwrite it here.
This means ESP-IDF's built-in panic handler is bypassed. If a fault occurs in
a ThreadX thread, it will hit our `_handle_exception` spin loop instead.

```asm
    li      t0, 0x800
    csrs    mie, t0
```
`li t0, 0x800` loads the immediate value 0x800 = 0b100000000000 into t0.
Bit 11 of the `mie` CSR is **MEIE** (Machine External Interrupt Enable).

`csrs mie, t0` sets bit 11 in `mie` without touching other bits.

Without this, the PLIC can assert a pending interrupt but the CPU will not take
it, because `mie.MEIE = 0` blocks all machine external interrupts.

The mie CSR has three interrupt enable bits we care about:
```
Bit 11: MEIE — machine external (PLIC-routed peripherals)
Bit 7:  MTIE — machine timer (mtime/mtimecmp, not used on ESP32-C6)
Bit 3:  MSIE — machine software (not used)
```

```asm
    call    _tx_port_setup_timer_interrupt
```
Calls our C function in `tx_timer_interrupt.c`. The `call` pseudo-instruction
expands to `auipc ra, offset_hi` + `jalr ra, ra, offset_lo`, which saves the
return address in `ra` and jumps to the function.

The C function configures SYSTIMER, INTMTX, and PLIC for the tick interrupt.

```asm
    LOAD    ra, 0(sp)
    addi    sp, sp, 16
    ret
```
Standard function epilogue. Restores `ra` from our stack frame, pops the frame,
and returns (`ret` = `jalr x0, ra, 0`).

---

### Free Memory Marker

```asm
    .section .data
    .global __tx_free_memory_start
__tx_free_memory_start:
    .word   0
```
A 4-byte global variable in the `.data` section. Its **address** is used as the
free memory marker — the actual content (zero) is irrelevant. The linker places
this in DRAM, and its address appears after all ThreadX static data.

---

### _tx_esp32c6_trap_handler

This is the unified handler for all machine-mode traps (interrupts + exceptions).
It follows a strict calling convention required by ThreadX's context save/restore
pair.

```asm
    .align 4
    .type _tx_esp32c6_trap_handler, @function
_tx_esp32c6_trap_handler:
```
`.align 4` ensures the handler is 4-byte aligned. This is required because
`mtvec` stores only the upper 30 bits of the address; the CPU assumes 4-byte
alignment.

`.type ..., @function` tells the assembler/linker this is a function (affects
`.size` and debug info).

```asm
    addi    sp, sp, -INT_FRAME_SIZE
```
`addi sp, sp, -128` on RV32. This allocates a 128-byte frame on the **current
stack**. At this point:
- If this is the first (non-nested) interrupt and a thread was running: sp is
  the thread's stack. The frame is allocated on the thread stack, then
  `_tx_thread_context_save` will switch sp to the system stack.
- If this is a nested interrupt: sp is already the system stack. The frame is
  allocated there.
- If the idle loop was interrupted: sp is the system stack.

```asm
    STORE   x1, 28*REGBYTES(sp)
```
`sw x1, 112(sp)` on RV32 (28 * 4 = 112).

`x1` is `ra` (return address). We must save it **before** calling
`_tx_thread_context_save` because `call` will overwrite `ra` with the return
address of that call.

Why offset 28*REGBYTES? This is the location where the ThreadX context save
code expects to find `ra`. Looking at `tx_thread_context_save.S`:
```
The code saves registers at these offsets within the frame:
  x5  (t0):  19*REGBYTES
  x6  (t1):  18*REGBYTES
  x7  (t2):  17*REGBYTES
  x8  (s0):  12*REGBYTES
  x10 (a0):  27*REGBYTES
  x11 (a1):  26*REGBYTES
  x12 (a2):  25*REGBYTES
  x13 (a3):  24*REGBYTES
  x14 (a4):  23*REGBYTES
  x15 (a5):  22*REGBYTES
  x16 (a6):  21*REGBYTES
  x17 (a7):  20*REGBYTES
  x28 (t3):  16*REGBYTES
  x29 (t4):  15*REGBYTES
  x30 (t5):  14*REGBYTES
  x31 (t6):  13*REGBYTES
  mepc:      30*REGBYTES

  x1 (ra):   NOT saved by context_save (it was already saved at 28*REGBYTES
             by the trap handler, and context_restore restores it from there)
```

The context_restore code later restores all registers from these offsets.
The overall frame layout must be consistent between the trap entry, context
save, and context restore.

```asm
    call    _tx_thread_context_save
```
Calls the upstream ThreadX function. It:
1. Saves all remaining scratch registers (t0-t6, a0-a7, s0) to the frame
2. Saves `mepc` to `30*REGBYTES(sp)`
3. Increments `_tx_thread_system_state` (interrupt nesting counter)
4. If first interrupt (non-nested) AND a thread was running:
   - Saves `sp` (the thread's stack pointer) into the thread's control block
   - Loads `sp` from `_tx_thread_system_stack_ptr` (the system stack)
5. Returns to us (via `ret` from context_save)

After `call _tx_thread_context_save` returns, `sp` points to the **system
stack** (unless we were nested or in idle). Our `t0`–`t6`, `a0`–`a7` etc.
are all safe to use.

```asm
    csrr    t0, mcause
    bltz    t0, _handle_interrupt
```
`csrr t0, mcause` reads the Machine Cause register. On RISC-V:
```
mcause [31]:    1 = interrupt, 0 = exception
mcause [30:0]:  cause code
```

`bltz t0, _handle_interrupt` branches if `t0 < 0` (i.e., bit 31 is set).
In 2's complement, bit 31 being set means the value is negative as a signed
32-bit integer. This is a clever way to test bit 31 without an explicit mask.

If bit 31 is 0 (exception), we fall through to:

```asm
_handle_exception:
    j       _handle_exception
```
An infinite loop. This is a placeholder — in production code you would call
the panic handler or save registers for a crash dump. For now any exception
(load fault, instruction fault, misaligned access, etc.) halts here.

**For audit**: The fact that we spin here means that bugs like null pointer
dereferences will halt the CPU silently. The UART may still work if the panic
happens in a thread (not during UART access). Consider adding a proper panic
handler that logs mcause, mepc, and the stack trace.

```asm
_handle_interrupt:
    li      t1, PLIC_MX_EIP_STATUS_REG
    lw      t2, 0(t1)
```
Loads `t1` with the PLIC status register address (0x2000100C).
`lw t2, 0(t1)` reads the 32-bit register value into `t2`.

Each bit N in this register indicates CPU interrupt line N is pending.
Since we configured line 7 for SYSTIMER, we expect bit 7 to be set.

Note: We use `lw` directly (not the `LOAD` macro) because this is a memory-
mapped I/O register, not a stack slot. The width is always 32-bit regardless
of REGBYTES.

```asm
    la      t3, _tx_esp32c6_timer_cpu_int
    lw      t3, 0(t3)
```
`_tx_esp32c6_timer_cpu_int` is a global variable (UINT, 32-bit) defined in
`tx_timer_interrupt.c` with value 7 (TIMER_CPU_INT_NUM).

First `la` loads the **address** of that variable into t3.
Then `lw t3, 0(t3)` loads the **value** (7) from that address into t3.

Reading through a variable (rather than using `.equ 7` directly) makes the
interrupt number configurable at runtime without recompiling the assembly.

```asm
    li      t4, 1
    sll     t4, t4, t3
```
`li t4, 1` loads the constant 1 into t4.
`sll t4, t4, t3` shifts t4 left by t3 bits: `t4 = 1 << 7 = 0x80`.

This creates a bitmask for CPU line 7.

```asm
    and     t5, t2, t4
    beqz    t5, _int_done
```
`and t5, t2, t4` ANDs the pending status (t2) with our bitmask (t4).
If bit 7 is set in t2, t5 will be non-zero.

`beqz t5, _int_done` branches to the done label if t5 == 0 (our interrupt
is NOT pending). This handles the case where some other CPU interrupt line
fires — we skip calling our ISR and just restore context.

```asm
    call    _tx_esp32c6_timer_isr
```
Calls our C ISR. This function:
1. Clears the SYSTIMER interrupt status register
2. Clears the PLIC edge latch for line 7
3. Calls `_tx_timer_interrupt()` (ThreadX internal tick processing)

`_tx_timer_interrupt()` wakes any threads whose `tx_thread_sleep()` count
has expired, and runs any periodic timer callbacks.

```asm
_int_done:
    j       _tx_thread_context_restore
```
`j` is a pseudo-instruction for an unconditional jump (`jal x0, offset`).

We **jump** to `_tx_thread_context_restore`, not **call** it. This is critical:
context_restore does not return — it ends with `mret` which returns directly
to the interrupted thread (or a different, higher-priority thread if a
scheduling decision was made during the interrupt).

---

## tx_thread_context_restore.S — What It Does

Source: `components/threadx/threadx/ports/risc-v64/gnu/src/tx_thread_context_restore.S`

This is the upstream file — not modified by us. Key behavior:

1. Decrements `_tx_thread_system_state` (nesting counter)
2. If still nested (`_tx_thread_system_state > 0`): just restore the frame
   from `sp` and `mret` (quick path for nested interrupts)
3. If first-level interrupt returning:
   a. Calls `_tx_thread_schedule()` to pick the highest-priority ready thread
   b. `_tx_thread_schedule` never returns — it loads the chosen thread's
      context and does `mret` to switch to it

This is the mechanism for preemptive scheduling: if `_tx_timer_interrupt()`
woke a higher-priority thread, `_tx_thread_schedule()` will switch to it
instead of returning to the thread that was interrupted.

---

## tx_thread_context_save.S — Frame Layout

The 32-slot (128-byte on RV32) frame is populated as follows:

```
sp + 0*4  = unused (could hold x2/sp, but sp is saved in TCB separately)
sp + 1*4  = unused
sp + 2*4  = unused
...
sp + 12*4 = x8  (s0)
sp + 13*4 = x31 (t6)
sp + 14*4 = x30 (t5)
sp + 15*4 = x29 (t4)
sp + 16*4 = x28 (t3)
sp + 17*4 = x7  (t2)
sp + 18*4 = x6  (t1)
sp + 19*4 = x5  (t0)
sp + 20*4 = x17 (a7)
sp + 21*4 = x16 (a6)
sp + 22*4 = x15 (a5)
sp + 23*4 = x14 (a4)
sp + 24*4 = x13 (a3)
sp + 25*4 = x12 (a2)
sp + 26*4 = x11 (a1)
sp + 27*4 = x10 (a0)
sp + 28*4 = x1  (ra) ← saved by our trap handler BEFORE calling context_save
sp + 29*4 = unused
sp + 30*4 = mepc     ← saved by context_save
sp + 31*4 = unused
```

Registers NOT in this frame (saved-registers per ABI, must be saved by the
thread itself in its own prologue): s1–s11 (x9, x18–x27). These are callee-
saved so they are preserved through the function calls inside the ISR.

---

## tx_thread_schedule.S — Thread Switch

Source: upstream file, not modified. Key points:

- Reads `_tx_thread_execute_ptr` to find which thread should run
- Loads that thread's saved `sp` from its Thread Control Block (TCB)
- Restores all registers from the thread's stack frame
- Does `mret` to resume the thread

The `mret` restores `mstatus.MIE` from `mstatus.MPIE`, re-enabling interrupts
as part of returning to the thread. This is why ThreadX threads run with
interrupts enabled by default.

---

## tx_thread_interrupt_control.S

Source: upstream file. Provides `_tx_thread_interrupt_control(UINT new_posture)`.

```asm
_tx_thread_interrupt_control:
    # a0 = new posture (TX_INT_ENABLE=1 or TX_INT_DISABLE=0)
    csrr    t0, mstatus
    andi    a0, a0, 8          # mask to MIE bit
    csrw    mstatus, a0        # write new MIE state
    andi    a0, t0, 8          # return old MIE state
    ret
```

This is what `TX_DISABLE` and `TX_RESTORE` macros expand to. They save/restore
the MIE bit in mstatus for critical sections.

---

## tx_thread_stack_build.S

Source: upstream file. Called when creating a new thread to set up its initial
stack frame so that `_tx_thread_schedule` can "restore" it into a running thread.

Key: it writes `mepc` = thread entry point, so when `mret` executes during
the first schedule-in, the CPU jumps to the thread function.

It also sets `mstatus.MPIE = 1` so that `mret` enables interrupts for the new
thread (since `mret` sets `mstatus.MIE = mstatus.MPIE`).
