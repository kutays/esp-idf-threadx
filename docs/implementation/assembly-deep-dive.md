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

---

## rtos_int_hooks.S — ESP-IDF Interrupt Dispatch Hooks

Source: `components/freertos/threadx/src/rtos_int_hooks.S` (our file).

This file provides `rtos_int_enter` and `rtos_int_exit` — two functions called by
ESP-IDF's `_interrupt_handler` (in `esp-idf/components/riscv/vectors.S`) for ALL
non-ThreadX interrupts (WiFi, Bluetooth, UART, esp_timer, GPIO, etc.).

### Why This File Exists

The ESP32-C6 has TWO completely separate interrupt paths:

```
Path A — ThreadX Timer (CPU line 17):
  vector[17] → _tx_esp32c6_trap_handler → _tx_thread_context_save
    → _tx_esp32c6_timer_isr → _tx_thread_context_restore

Path B — All ESP-IDF Interrupts (CPU lines ≠ 17):
  vector[N] → _interrupt_handler → rtos_int_enter
    → _global_interrupt_handler → rtos_int_exit
```

Path A uses ThreadX's native interrupt context save/restore (saves all registers in a
32*REGBYTES "interrupt frame", type=1). Path B uses ESP-IDF's `save_general_regs` /
`restore_general_regs` macros (different layout, `RV_STK_*` offsets).

`rtos_int_hooks.S` bridges Path B into the ThreadX world by:
1. Managing `_tx_thread_system_state` (ThreadX ISR nesting counter)
2. Managing `port_uxInterruptNesting` (FreeRTOS compat counter)
3. Switching to/from the ISR stack
4. Checking for preemption on ISR exit

### Calling Convention

From ESP-IDF's `vectors.S`, the hooks are called like this:

```asm
_interrupt_handler:                     # vectors.S (ESP-IDF code, NOT ours)
    save_general_regs                   # Push all 31 regs (RV_STK layout)
    csrr  s1, mcause                   # s1 = interrupt cause (callee-saved!)
    csrr  s2, mstatus                  # s2 = saved mstatus  (callee-saved!)
    call  rtos_int_enter               # OUR HOOK — returns "context" in a0
    mv    s4, a0                       # s4 = context        (callee-saved!)

    # ... raise PLIC threshold, enable MIE, dispatch ISR, disable MIE ...

    mv    a0, s2                       # a0 = saved mstatus
    mv    a1, s4                       # a1 = context from rtos_int_enter
    call  rtos_int_exit                # OUR HOOK — returns mstatus in a0

    csrw  mstatus, a0                  # Restore mstatus (MIE=0 at this point)
    restore_general_regs               # Pop all 31 regs
    mret                               # Return to interrupted code
```

Key: `_interrupt_handler` saves mcause/mstatus in `s1`/`s2` (callee-saved registers).
These survive all function calls including our hooks. After `rtos_int_exit` returns,
`_interrupt_handler` restores mstatus from `a0` (our return value), restores all
registers, and does `mret`.

### rtos_int_enter — Annotated Line by Line

```asm
rtos_int_enter:
    lw      t0, port_xSchedulerRunning   # t0 = scheduler running flag
    #   Absolute address load — port_xSchedulerRunning is a .data symbol.
    #   The assembler generates a lui+lw pair (position-dependent).
    beqz    t0, .Lenter_end              # If scheduler not running, skip everything.
    #   Before tx_kernel_enter() completes, we must not touch any ThreadX
    #   state. Just return 0 and let _interrupt_handler continue.
```

**Increment FreeRTOS compat nesting counter:**
```asm
    la      t3, port_uxInterruptNesting  # t3 = address of nesting counter
    #   'la' = load address (lui+addi pair). We need the ADDRESS to both
    #   read and write the counter.
    lw      t4, 0(t3)                   # t4 = current nesting depth
    addi    t5, t4, 1                   # t5 = nesting + 1
    sw      t5, 0(t3)                   # Store incremented nesting
```

**Increment ThreadX system state (Bug 36 fix):**
```asm
    la      t3, _tx_thread_system_state  # t3 = address of system state
    lw      t5, 0(t3)                   # t5 = current system state
    addi    t5, t5, 1                   # t5 = system_state + 1
    sw      t5, 0(t3)                   # Store incremented state
    #   CRITICAL: ThreadX APIs check _tx_thread_system_state to determine
    #   if they're called from ISR context. If this is 0 during an ISR,
    #   tx_semaphore_put() thinks it's in thread context and calls
    #   _tx_thread_system_return(), corrupting the scheduler (Bug 36).
```

**Stack switch (first-level interrupt only):**
```asm
    bnez    t4, .Lenter_end              # t4 = nesting BEFORE increment.
    #   If t4 > 0, we're nested — already on ISR stack, skip switch.

    lw      t0, pxCurrentTCBs           # t0 = pointer to current TCB
    #   Absolute address load of the TCB pointer variable.
    beqz    t0, .Lenter_end              # Defensive: no current task
    sw      sp, 0(t0)                   # TCB->pxTopOfStack = sp
    #   Save current stack pointer into the TCB. FreeRTOS convention:
    #   offset 0 of TCB = pxTopOfStack. vectors.S relies on this for
    #   context restore.
    lw      sp, xIsrStackTop            # Switch SP to top of ISR stack
    #   ISR stack is a separate 4KB region (.bss). Stack grows DOWN,
    #   so xIsrStackTop = xIsrStack + 4096.
```

**Return:**
```asm
.Lenter_end:
    li      a0, 0                       # Return 0 (no coprocessors on C6)
    ret                                 # Return to _interrupt_handler
```

### rtos_int_exit — Annotated Line by Line (Bug 37 Fix)

This is the critical function. It must:
1. Restore nesting counters
2. Switch back to the task stack (if exiting last ISR level)
3. Check if a higher-priority thread became ready during the ISR
4. Trigger a context switch if needed — WITHOUT corrupting registers

```asm
rtos_int_exit:
    mv      s11, a0                     # s11 = saved mstatus
    #   a0 comes from _interrupt_handler: it's the mstatus value saved on
    #   interrupt entry (before the ISR ran). We need to return this value
    #   at the end so _interrupt_handler can restore it via csrw mstatus.
    #
    #   WHY s11? Because s11 is callee-saved (RISC-V ABI). It survives:
    #     - Our function calls (obvious)
    #     - _tx_thread_system_preempt_check()
    #     - _tx_thread_system_return() (saves s11 in solicited frame)
    #     - The entire scheduler round-trip
    #     - _tx_thread_synch_return (restores s11 from solicited frame)
    #   When the original thread resumes, s11 still holds our mstatus.
```

**Check scheduler running:**
```asm
    lw      t0, port_xSchedulerRunning
    beqz    t0, .Lexit_end              # Not running — return mstatus
```

**Decrement FreeRTOS compat nesting:**
```asm
    la      t2, port_uxInterruptNesting
    lw      t3, 0(t2)                   # t3 = current nesting
    beqz    t3, .Lskip_dec              # Guard: don't go below 0
    addi    t3, t3, -1
    sw      t3, 0(t2)                   # t3 now = nesting AFTER decrement
.Lskip_dec:
```

**Decrement ThreadX system state:**
```asm
    la      t2, _tx_thread_system_state
    lw      t4, 0(t2)
    beqz    t4, .Lskip_sys_dec          # Guard: don't go below 0
    addi    t4, t4, -1
    sw      t4, 0(t2)
.Lskip_sys_dec:
```

**Check if we're still nested:**
```asm
    bnez    t3, .Lexit_end              # t3 = nesting AFTER decrement.
    #   If still > 0, we're inside a nested ISR. Stay on ISR stack,
    #   don't check for preemption (an outer rtos_int_exit will handle it).
```

**The preemption check (Bug 37 fix):**
```asm
    # ═══════════════════════════════════════════════════════════════════
    # EXITING LAST ISR LEVEL
    #
    # At this point:
    #   _tx_thread_system_state = 0   (just decremented)
    #   We're still on the ISR stack  (rtos_int_enter switched us)
    #
    # _tx_thread_system_preempt_check() is a C function that:
    #   1. Reads _tx_thread_system_state + _tx_thread_preempt_disable
    #   2. If both are 0 AND execute_ptr != current_ptr:
    #      Calls _tx_thread_system_return() → scheduler → other thread runs
    #   3. When our thread resumes: returns normally
    #
    # We need to save/restore ra because 'call' overwrites it.
    # ═══════════════════════════════════════════════════════════════════

    addi    sp, sp, -16                 # Allocate 16 bytes (aligned)
    sw      ra, 0(sp)                   # Save return address
    call    _tx_thread_system_preempt_check
    #   This call may:
    #   (a) Return immediately — no preemption needed. Fast path.
    #   (b) Call _tx_thread_system_return() internally, which:
    #       - Saves a solicited frame (type=0) on THIS stack
    #       - Stores SP in our TCB
    #       - Switches to system stack
    #       - Jumps to _tx_thread_schedule (never returns directly)
    #       - When we're re-scheduled: _tx_thread_synch_return restores
    #         our callee-saved regs (including s11 = mstatus) and does
    #         `ret` which returns HERE, to the lw instruction below.
    lw      ra, 0(sp)                   # Restore return address
    addi    sp, sp, 16                  # Free stack frame
```

**Return:**
```asm
.Lexit_end:
    mv      a0, s11                     # Return mstatus in a0
    #   _interrupt_handler will: csrw mstatus, a0 (sets MIE=0, MPIE=saved)
    #   then: restore_general_regs, mret (MPIE→MIE, restoring original state)
    ret
```

### Why This Works: The Callee-Saved Register Chain

The entire mechanism relies on callee-saved register preservation through multiple
function boundaries:

```
_interrupt_handler saves mstatus in s2        (callee-saved)
  ↓
rtos_int_exit copies s2 → a0, then a0 → s11  (callee-saved)
  ↓
_tx_thread_system_preempt_check preserves s11 (C calling convention)
  ↓
_tx_thread_system_return saves s11 at 1*REGBYTES(sp) in solicited frame
  ↓
... scheduler runs other thread(s) ...
  ↓
_tx_thread_synch_return loads s11 from 1*REGBYTES(sp)
  ↓
ret → back in rtos_int_exit, s11 still = original mstatus
  ↓
rtos_int_exit returns a0 = s11 = original mstatus
  ↓
_interrupt_handler: csrw mstatus, a0 → restore_general_regs → mret
```

No register is lost. No context format conversion is needed. The solicited frame
contains only callee-saved registers, which is exactly what we need because we're
inside a C function call chain.

---

## tx_thread_system_return.S — Solicited Context Save

Source: upstream file (risc-v64/gnu port). Called when a thread voluntarily gives up
the CPU (via `_tx_thread_system_preempt_check`, `tx_thread_sleep`, etc.).

The key difference from interrupt context save:

| | Interrupt Frame (type=1) | Solicited Frame (type=0) |
|---|---|---|
| Size | 32*REGBYTES (128 bytes on RV32) | 16*REGBYTES (64 bytes on RV32) |
| Registers saved | ALL 31 + mepc | Only s0-s11, ra, mstatus (14 regs) |
| Resume mechanism | Full restore + `mret` | Callee-saved restore + `ret` |
| Who creates it | `_tx_thread_context_save` | `_tx_thread_system_return` |

**Annotated code (stripped of floating-point for clarity):**

```asm
_tx_thread_system_return:
    addi    sp, sp, -16*REGBYTES        # Allocate solicited frame

    STORE   x0,  0(sp)                  # type = 0 (solicited marker)
    #   _tx_thread_schedule checks this: 0 → synch_return, non-0 → full restore

    STORE   x1,  13*REGBYTES(sp)        # Save ra  (return address)
    STORE   x8,  12*REGBYTES(sp)        # Save s0  (frame pointer)
    STORE   x9,  11*REGBYTES(sp)        # Save s1
    STORE   x18, 10*REGBYTES(sp)        # Save s2
    STORE   x19,  9*REGBYTES(sp)        # Save s3
    STORE   x20,  8*REGBYTES(sp)        # Save s4
    STORE   x21,  7*REGBYTES(sp)        # Save s5
    STORE   x22,  6*REGBYTES(sp)        # Save s6
    STORE   x23,  5*REGBYTES(sp)        # Save s7
    STORE   x24,  4*REGBYTES(sp)        # Save s8
    STORE   x25,  3*REGBYTES(sp)        # Save s9
    STORE   x26,  2*REGBYTES(sp)        # Save s10
    STORE   x27,  1*REGBYTES(sp)        # Save s11  ← THIS IS OUR mstatus!
    csrr    t0, mstatus
    STORE   t0, 14*REGBYTES(sp)         # Save mstatus (interrupt enable state)

    csrci   mstatus, 0xF                # Disable all interrupts

    # Save SP into current thread's TCB:
    la      t0, _tx_thread_current_ptr
    LOAD    t1, 0(t0)                   # t1 = current thread TCB
    STORE   sp, 2*REGBYTES(t1)          # TCB->tx_thread_stack_ptr = sp

    # Switch to system stack:
    la      t2, _tx_thread_system_stack_ptr
    LOAD    sp, 0(t2)                   # sp = system stack

    # Clear current thread pointer:
    STORE   x0, 0(t0)                   # _tx_thread_current_ptr = NULL

    # Jump to scheduler (never returns here):
    la      t2, _tx_thread_schedule
    jr      t2
```

### tx_thread_schedule.S — The Scheduler + synch_return

**Scheduler loop:**
```asm
_tx_thread_schedule:
    csrsi   mstatus, 0x08               # Enable interrupts (so timer tick works)

    la      t0, _tx_thread_execute_ptr
_tx_thread_schedule_loop:
    LOAD    t1, 0(t0)                   # t1 = next thread to run
    beqz    t1, _tx_thread_schedule_loop # Spin until a thread is ready
    #   This loop runs with MIE=1, so timer interrupts can fire and
    #   wake threads (e.g., tx_thread_sleep expiry).

    csrci   mstatus, 0x08               # Disable interrupts for context switch

    la      t0, _tx_thread_current_ptr
    STORE   t1, 0(t0)                   # _tx_thread_current_ptr = t1

    LOAD    sp, 2*REGBYTES(t1)          # sp = thread's saved stack pointer
    LOAD    t2, 0(sp)                   # t2 = stack type at offset 0
    beqz    t2, _tx_thread_synch_return  # type 0 → solicited return
    #   type ≠ 0 → interrupt frame → full register restore + mret
    #   (full restore code omitted for brevity — see tx_thread_schedule.S)
```

**Synchronous (solicited) return:**
```asm
_tx_thread_synch_return:
    LOAD    x1,  13*REGBYTES(sp)        # Restore ra
    LOAD    x8,  12*REGBYTES(sp)        # Restore s0
    LOAD    x9,  11*REGBYTES(sp)        # Restore s1
    LOAD    x18, 10*REGBYTES(sp)        # Restore s2
    LOAD    x19,  9*REGBYTES(sp)        # Restore s3
    LOAD    x20,  8*REGBYTES(sp)        # Restore s4
    LOAD    x21,  7*REGBYTES(sp)        # Restore s5
    LOAD    x22,  6*REGBYTES(sp)        # Restore s6
    LOAD    x23,  5*REGBYTES(sp)        # Restore s7
    LOAD    x24,  4*REGBYTES(sp)        # Restore s8
    LOAD    x25,  3*REGBYTES(sp)        # Restore s9
    LOAD    x26,  2*REGBYTES(sp)        # Restore s10
    LOAD    x27,  1*REGBYTES(sp)        # Restore s11  ← OUR mstatus IS BACK!
    LOAD    t0,  14*REGBYTES(sp)        # Restore mstatus
    csrw    mstatus, t0                 # Write mstatus (may enable interrupts)
    addi    sp, sp, 16*REGBYTES         # Free solicited frame
    ret                                 # Return to caller of system_return
    #   In our case: returns to rtos_int_exit, right after the
    #   'call _tx_thread_system_preempt_check' instruction.
```

---

## Dual Interrupt Path Summary

```
                    ┌─────────────────────┐
                    │   CPU Interrupt      │
                    │   Vector Table       │
                    └─────┬───────┬───────┘
                          │       │
                   vector[17]   vector[N≠17]
                          │       │
                   ┌──────┴──┐  ┌─┴──────────────┐
                   │ ThreadX │  │ ESP-IDF         │
                   │ Timer   │  │ _interrupt_handler│
                   │ Path A  │  │ Path B          │
                   └────┬────┘  └────┬────────────┘
                        │            │
              context_save    rtos_int_enter
              (system_state++) (system_state++)
                        │            │
              timer_isr        _global_interrupt_handler
                        │      (WiFi, BT, UART, esp_timer...)
                        │            │
              context_restore  rtos_int_exit
              (system_state--) (system_state--)
              checks preempt   checks preempt via
              via execute_ptr  _tx_thread_system_preempt_check
                        │            │
                   ┌────┴────────────┴────┐
                   │   Both paths correctly│
                   │   trigger context     │
                   │   switch when needed  │
                   └──────────────────────┘
```

Both paths maintain `_tx_thread_system_state` in sync. Both check for preemption
when exiting the last ISR nesting level. Cross-path nesting works correctly:
- ESP-IDF ISR → ThreadX timer nests: state goes 1→2→1→0 (correct)
- ThreadX timer → ESP-IDF ISR nests: state goes 1→2→1→0 (correct)
