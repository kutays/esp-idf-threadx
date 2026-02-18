/*
 * tx_initialize_low_level.S — ESP32-C6 low-level ThreadX initialization
 *
 * This replaces the generic RISC-V tx_initialize_low_level.S from the
 * ThreadX port. It handles:
 *   1. Saving the system stack pointer for ISR use
 *   2. Setting up the first-available-memory pointer
 *   3. Installing a proper vectored MTVEC table for ESP32-C6
 *   4. Enabling machine external interrupts (mie.MEIE)
 *   5. Calling the C timer setup function for SYSTIMER
 *
 * ESP32-C6 MTVEC DISPATCH (BUG FIX):
 *   The ESP32-C6 CPU ALWAYS uses vectored dispatch regardless of the mtvec
 *   mode bits. Specifically (from vectors_intc.S comment in ESP-IDF):
 *
 *     "CPU only uses the 24 MSBs of the MTVEC, i.e. (MTVEC & 0xffffff00)"
 *
 *   Dispatch rules:
 *     Exception      → jump to (MTVEC & 0xffffff00) + 0      = vector[0]
 *     Interrupt N    → jump to (MTVEC & 0xffffff00) + N*4    = vector[N]
 *
 *   Therefore mtvec MUST point to a 256-byte aligned vector table where
 *   each entry is a 4-byte jump instruction. Installing a plain function
 *   address in mtvec DOES NOT WORK — interrupts jump into the middle of
 *   the handler code rather than the entry point.
 *
 *   We use CPU interrupt line 7 for SYSTIMER, so:
 *     vector[0] → _tx_esp32c6_exception_entry (CPU exception → abort())
 *     vector[7] → _tx_esp32c6_trap_handler    (SYSTIMER tick)
 *
 * ESP32-C6 uses SOC_INT_PLIC_SUPPORTED. Machine-mode external interrupts
 * are routed through the PLIC at 0x20001000. The mie.MEIE CSR bit (bit 11)
 * must be set to allow the PLIC to deliver interrupts to the CPU.
 */

#include "tx_port.h"

    .global _tx_initialize_low_level
    .global _tx_thread_context_save
    .global _tx_thread_context_restore

    /* Symbols provided by the linker */
    .extern _tx_initialize_unused_memory
    .extern _tx_thread_system_stack_ptr
    .extern _tx_port_setup_timer_interrupt
    .extern _tx_esp32c6_timer_isr
    .extern _tx_esp32c6_timer_cpu_int

    /*
     * PLIC machine-mode EIP status register.
     * DR_REG_PLIC_MX_BASE = 0x20001000, EIP_STATUS at offset 0x0C
     */
    .equ PLIC_MX_EIP_STATUS_REG, 0x2000100C

    /* Interrupt stack frame size: 32 registers * REGBYTES (no FP on ESP32-C6) */
    .equ INT_FRAME_SIZE, (32 * REGBYTES)

/* =========================================================================
 * Vector table
 *
 * MUST be 256-byte aligned — ESP32-C6 uses only the top 24 bits of MTVEC.
 * MUST use 4-byte instructions — .option norvc disables compressed encoding.
 *
 * Each entry is a `j` to a handler. The CPU dispatches to vector[N] for
 * interrupt line N, and to vector[0] for exceptions.
 *
 * We place this in .iram0.text so it is always in IRAM (accessible during
 * interrupts without needing the flash/cache to be active).
 * ========================================================================= */
    .section .iram0.text,"ax",@progbits
    .balign 256

    .option push
    .option norvc       /* Force 4-byte j instructions — critical for table layout */

    .global _tx_esp32c6_vector_table
    .type   _tx_esp32c6_vector_table, @function
_tx_esp32c6_vector_table:
    j   _tx_esp32c6_exception_entry /* 0: exception entry */
    j   _tx_esp32c6_unused_int      /* 1: unused CPU interrupt */
    j   _tx_esp32c6_unused_int      /* 2: unused CPU interrupt */
    j   _tx_esp32c6_unused_int      /* 3: unused CPU interrupt */
    j   _tx_esp32c6_unused_int      /* 4: unused CPU interrupt */
    j   _tx_esp32c6_unused_int      /* 5: unused CPU interrupt */
    j   _tx_esp32c6_unused_int      /* 6: unused CPU interrupt */
    j   _tx_esp32c6_trap_handler    /* 7: SYSTIMER tick (CPU interrupt line 7) */
    j   _tx_esp32c6_unused_int      /* 8: unused */
    j   _tx_esp32c6_unused_int      /* 9: unused */
    j   _tx_esp32c6_unused_int      /* 10: unused */
    j   _tx_esp32c6_unused_int      /* 11: unused */
    j   _tx_esp32c6_unused_int      /* 12: unused */
    j   _tx_esp32c6_unused_int      /* 13: unused */
    j   _tx_esp32c6_unused_int      /* 14: unused */
    j   _tx_esp32c6_unused_int      /* 15: unused */
    j   _tx_esp32c6_unused_int      /* 16: unused */
    j   _tx_esp32c6_unused_int      /* 17: unused */
    j   _tx_esp32c6_unused_int      /* 18: unused */
    j   _tx_esp32c6_unused_int      /* 19: unused */
    j   _tx_esp32c6_unused_int      /* 20: unused */
    j   _tx_esp32c6_unused_int      /* 21: unused */
    j   _tx_esp32c6_unused_int      /* 22: unused */
    j   _tx_esp32c6_unused_int      /* 23: unused */
    j   _tx_esp32c6_unused_int      /* 24: unused (ETS_INT_WDT_INUM in FreeRTOS) */
    j   _tx_esp32c6_unused_int      /* 25: unused */
    j   _tx_esp32c6_unused_int      /* 26: unused */
    j   _tx_esp32c6_unused_int      /* 27: unused */
    j   _tx_esp32c6_unused_int      /* 28: unused */
    j   _tx_esp32c6_unused_int      /* 29: unused */
    j   _tx_esp32c6_unused_int      /* 30: unused */
    j   _tx_esp32c6_unused_int      /* 31: unused */

    .option pop

/*
 * _tx_esp32c6_exception_entry
 *
 * CPU exceptions (illegal instruction, misaligned access, stack overflow, etc.)
 * arrive here via vector[0]. We switch to a known-good stack and call abort()
 * so the ESP-IDF panic handler can produce a proper crash dump.
 *
 * We do NOT run _tx_thread_context_save first — if the exception was caused by
 * a stack overflow the thread stack is already corrupt, and attempting to save
 * to it would cause further corruption before we get any output.
 */
    .extern _tx_thread_system_stack_ptr
    .extern abort
    .type _tx_esp32c6_exception_entry, @function
_tx_esp32c6_exception_entry:
    /* Switch to system stack so abort() has a valid stack to run on */
    la      t0, _tx_thread_system_stack_ptr
    LOAD    sp, 0(t0)
    call    abort           /* noreturn — triggers ESP-IDF panic handler */
    j       _tx_esp32c6_exception_entry  /* should never reach here */
    .size _tx_esp32c6_exception_entry, . - _tx_esp32c6_exception_entry

/* Catch-all for any interrupt line we didn't register */
    .type _tx_esp32c6_unused_int, @function
_tx_esp32c6_unused_int:
    j   _tx_esp32c6_unused_int  /* spin — should never arrive here */
    .size _tx_esp32c6_unused_int, . - _tx_esp32c6_unused_int


/* =========================================================================
 * _tx_initialize_low_level
 *
 * Called from tx_initialize_kernel_enter() before tx_application_define().
 * At this point we're running on the ESP-IDF startup stack.
 * ========================================================================= */
    .section .text
    .align 4
    .type _tx_initialize_low_level, @function
_tx_initialize_low_level:

    /* Save return address */
    addi    sp, sp, -16
    STORE   ra, 0(sp)

    /*
     * 1. Save the current stack pointer for ISR use.
     *    ThreadX ISRs run on the system stack, not the thread stack.
     */
    la      t0, _tx_thread_system_stack_ptr
    STORE   sp, 0(t0)

    /*
     * 2. Set up first unused memory pointer.
     *    We use a small data section symbol as the free memory marker.
     */
    la      t0, _tx_initialize_unused_memory
    la      t1, __tx_free_memory_start
    STORE   t1, 0(t0)

    /*
     * 3. Install our vector table in mtvec.
     *
     *    ESP32-C6 ALWAYS uses vectored dispatch:
     *      exception    → table[0]  (= table base address)
     *      interrupt N  → table[N]  (= table base + N*4)
     *    The table must be 256-byte aligned (CPU ignores lowest 8 bits).
     *
     *    We point mtvec at _tx_esp32c6_vector_table which is .balign 256.
     */
    la      t0, _tx_esp32c6_vector_table
    csrw    mtvec, t0

    /*
     * 4. Enable machine external interrupts in mie CSR.
     *    Bit 11 (MEIE) allows the PLIC to deliver external interrupts.
     *    Without this, no peripheral interrupt (including SYSTIMER) will fire.
     */
    li      t0, 0x800       /* MEIE = bit 11 */
    csrs    mie, t0

    /*
     * 5. Call C function to set up SYSTIMER for tick interrupts.
     *    This configures the INTMTX and PLIC registers.
     */
    call    _tx_port_setup_timer_interrupt

    /* Restore and return */
    LOAD    ra, 0(sp)
    addi    sp, sp, 16
    ret

    .size _tx_initialize_low_level, . - _tx_initialize_low_level


/* Free memory marker */
    .section .data
    .global __tx_free_memory_start
__tx_free_memory_start:
    .word   0

/* =========================================================================
 * _tx_esp32c6_trap_handler
 *
 * Unified trap/interrupt handler for ThreadX on ESP32-C6.
 *
 * Reached via the vector table:
 *   - vector[7]: CPU interrupt line 7 / SYSTIMER tick (mcause bit 31 = 1)
 *
 * MUST be in .iram0.text — the vector table entries are 4-byte `j` (JAL)
 * instructions with ±1MB range. IRAM is ~0x40800000, flash is ~0x42000000,
 * so the handler must be in IRAM for the vector table jump to reach it.
 *
 * The ThreadX context save/restore calling convention requires:
 *   1. Allocate interrupt stack frame (32 * REGBYTES = 128 bytes on RV32)
 *   2. Save x1 (RA) at offset 28*REGBYTES into the frame
 *      (context_save will overwrite it via its own 'call' prologue)
 *   3. Call _tx_thread_context_save (saves all other scratch regs + mepc)
 *   4. Handle the interrupt
 *   5. Jump to _tx_thread_context_restore (does NOT return — ends with mret)
 *
 * Note on _tx_thread_context_restore: it lives in flash (.text). We cannot
 * reach it with a plain `j` (JAL, ±1MB) from IRAM. We use `la + jr`
 * (register-indirect jump) which has unlimited range.
 * ========================================================================= */
    .section .iram0.text,"ax",@progbits
    .align 4
    .type _tx_esp32c6_trap_handler, @function
_tx_esp32c6_trap_handler:

    /* Step 1: Allocate interrupt stack frame */
    addi    sp, sp, -INT_FRAME_SIZE

    /* Step 2: Save RA (x1) — context_save will overwrite it via 'call' */
    STORE   x1, 28*REGBYTES(sp)

    /* Step 3: Save context (stores all scratch regs, mepc, handles nesting) */
    call    _tx_thread_context_save

    /* Now running on system stack (if first interrupt) or same stack (if nested) */

    /* Read mcause to determine interrupt vs exception.
     * Exceptions should not reach here — they go via vector[0] to
     * _tx_esp32c6_exception_entry which calls abort(). If we somehow
     * arrive here for an exception, redirect. */
    csrr    t0, mcause
    bltz    t0, _handle_interrupt     /* bit 31 set = interrupt */
    j       _tx_esp32c6_exception_entry /* exception: should not happen */

_handle_interrupt:
    /*
     * External interrupt. Read the PLIC machine-mode EIP status register
     * to find which CPU interrupt line fired.
     */
    li      t1, PLIC_MX_EIP_STATUS_REG
    lw      t2, 0(t1)

    /*
     * Check if our SYSTIMER CPU interrupt bit is set.
     * Load the configured interrupt number and create bit mask.
     */
    la      t3, _tx_esp32c6_timer_cpu_int
    lw      t3, 0(t3)
    li      t4, 1
    sll     t4, t4, t3          /* t4 = (1 << cpu_int_num) */
    and     t5, t2, t4
    beqz    t5, _int_done       /* not our timer interrupt */

    /* Call the ThreadX timer ISR (clears interrupt + calls _tx_timer_interrupt) */
    call    _tx_esp32c6_timer_isr

_int_done:
    /* Step 5: Restore context — this does NOT return (ends with mret).
     * _tx_thread_context_restore is in flash (.text). We cannot use `j`
     * (JAL, ±1MB) from IRAM. Use la+jr for unlimited-range tail-call. */
    la      t0, _tx_thread_context_restore
    jr      t0

    .size _tx_esp32c6_trap_handler, . - _tx_esp32c6_trap_handler
