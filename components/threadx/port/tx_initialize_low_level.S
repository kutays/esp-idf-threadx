// SPDX-License-Identifier: Apache-2.0
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
 *   Lines 1, 3, 4, 6, 7 are RESERVED on ESP32-C6 (CLINT/Wi-Fi bound).
 *   Lines 6, 7, 11, 15, 16, 29 are "effectively disabled" by the INTMTX hardware.
 *   We use CPU interrupt line 17 for SYSTIMER:
 *     vector[0]  → _tx_esp32c6_exception_entry (CPU exception → abort())
 *     vector[17] → _tx_esp32c6_trap_handler    (SYSTIMER tick)
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

    /* ESP-IDF's generic interrupt dispatcher (from riscv/vectors.S).
     * All non-ThreadX interrupts (WiFi, BLE, etc.) are routed here.
     * It saves/restores FreeRTOS-style context via rtos_int_enter/exit,
     * then dispatches to the handler registered via intr_handler_set(). */
    .extern _interrupt_handler

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
    j   _interrupt_handler          /* 1: ESP-IDF generic dispatch (WiFi, etc.) */
    j   _interrupt_handler          /* 2: ESP-IDF generic dispatch */
    j   _interrupt_handler          /* 3: ESP-IDF generic dispatch */
    j   _interrupt_handler          /* 4: ESP-IDF generic dispatch */
    j   _interrupt_handler          /* 5: ESP-IDF generic dispatch */
    j   _interrupt_handler          /* 6: reserved (permanently disabled) */
    j   _interrupt_handler          /* 7: reserved (CLINT machine timer) */
    j   _interrupt_handler          /* 8: ESP-IDF generic dispatch */
    j   _interrupt_handler          /* 9: ESP-IDF generic dispatch */
    j   _interrupt_handler          /* 10: ESP-IDF generic dispatch */
    j   _interrupt_handler          /* 11: ESP-IDF generic dispatch */
    j   _interrupt_handler          /* 12: ESP-IDF generic dispatch */
    j   _interrupt_handler          /* 13: ESP-IDF generic dispatch */
    j   _interrupt_handler          /* 14: ESP-IDF generic dispatch */
    j   _interrupt_handler          /* 15: ESP-IDF generic dispatch */
    j   _interrupt_handler          /* 16: ESP-IDF generic dispatch */
    j   _tx_esp32c6_trap_handler    /* 17: SYSTIMER tick (ThreadX context save/restore) */
    j   _interrupt_handler          /* 18: ESP-IDF generic dispatch */
    j   _interrupt_handler          /* 19: ESP-IDF generic dispatch */
    j   _interrupt_handler          /* 20: ESP-IDF generic dispatch */
    j   _interrupt_handler          /* 21: ESP-IDF generic dispatch */
    j   _interrupt_handler          /* 22: ESP-IDF generic dispatch */
    j   _interrupt_handler          /* 23: ESP-IDF generic dispatch */
    j   _interrupt_handler          /* 24: ESP-IDF generic dispatch */
    j   _interrupt_handler          /* 25: ESP-IDF generic dispatch */
    j   _interrupt_handler          /* 26: ESP-IDF generic dispatch */
    j   _interrupt_handler          /* 27: ESP-IDF generic dispatch */
    j   _interrupt_handler          /* 28: ESP-IDF generic dispatch */
    j   _interrupt_handler          /* 29: ESP-IDF generic dispatch */
    j   _interrupt_handler          /* 30: ESP-IDF generic dispatch */
    j   _interrupt_handler          /* 31: ESP-IDF generic dispatch */

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

/* _tx_esp32c6_unused_int is no longer needed — all non-17 vectors route
 * to _interrupt_handler (ESP-IDF generic dispatch). Kept as a safety net
 * in case something references it. */
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

    /*
     * 0. Disable global machine interrupts for the duration of initialization.
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
     *
     *    Bug 29: documented in docs/implementation/bug-history.md
     */
    csrci   mstatus, 0x8        /* clear MIE (bit 3) — disable global interrupts */

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
 * SYSTIMER tick handler — reached exclusively via vector[7].
 *
 * In vectored mode the CPU dispatches interrupt N to vector[N], and
 * exceptions to vector[0]. Because vector[7] is our SYSTIMER entry, we
 * know unconditionally that this is the SYSTIMER tick. No mcause check
 * and no PLIC EIP_STATUS check are needed.
 *
 * (Previous design read PLIC_MX_EIP_STATUS before calling the ISR. On
 *  ESP32-C6 for edge-triggered interrupts, the PLIC clears the EIP bit
 *  when the CPU takes the interrupt — so the bit was always 0 by the time
 *  we read it and the ISR was silently skipped every single time.)
 *
 * MUST be in .iram0.text — the vector table `j` entries have ±1MB range.
 * IRAM ~0x40800000 to flash ~0x42000000 is ~32 MB — exceeds JAL range.
 *
 * _tx_thread_context_restore is in flash (.text); reach it with la+jr
 * (unlimited range) instead of j (±1 MB).
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

    /* Step 4: Run the SYSTIMER ISR — clears the alarm and calls
     *         _tx_timer_interrupt() to advance the ThreadX tick. */
    call    _tx_esp32c6_timer_isr

    /* Step 5: Restore context — does NOT return (ends with mret).
     * la+jr gives unlimited range for the IRAM → flash jump. */
    la      t0, _tx_thread_context_restore
    jr      t0

    .size _tx_esp32c6_trap_handler, . - _tx_esp32c6_trap_handler
