/*
 * tx_initialize_low_level.S — ESP32-C6 low-level ThreadX initialization
 *
 * This replaces the generic RISC-V tx_initialize_low_level.S from the
 * ThreadX port. It handles:
 *   1. Saving the system stack pointer for ISR use
 *   2. Setting up the first-available-memory pointer
 *   3. Installing the ThreadX trap/exception handler in mtvec
 *   4. Enabling machine external interrupts (mie.MEIE)
 *   5. Calling the C timer setup function for SYSTIMER
 *
 * ESP32-C6 uses SOC_INT_PLIC_SUPPORTED. Machine-mode external interrupts
 * are routed through the PLIC at 0x20001000. The mie.MEIE CSR bit (bit 11)
 * must be set to allow the PLIC to deliver interrupts to the CPU.
 */

#include "tx_port.h"

    .section .text
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

/*
 * _tx_initialize_low_level
 *
 * Called from tx_initialize_kernel_enter() before tx_application_define().
 * At this point we're running on the ESP-IDF startup stack.
 */
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
     * 3. Install our trap handler in mtvec (direct mode, bit 0 = 0).
     */
    la      t0, _tx_esp32c6_trap_handler
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

    .section .text

/*
 * _tx_esp32c6_trap_handler
 *
 * Unified trap/interrupt handler for ThreadX on ESP32-C6.
 *
 * The ThreadX context save/restore calling convention requires:
 *   1. Allocate interrupt stack frame (32 * REGBYTES)
 *   2. Save x1 (RA) at offset 28*REGBYTES into the frame
 *   3. Call _tx_thread_context_save (saves all other scratch regs)
 *   4. Handle the interrupt
 *   5. Call _tx_thread_context_restore (does not return — mret)
 */
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

    /* Read mcause to determine interrupt vs exception */
    csrr    t0, mcause
    bltz    t0, _handle_interrupt     /* bit 31 set = interrupt */

    /* Exception — for now just spin (shouldn't happen in normal operation) */
_handle_exception:
    j       _handle_exception

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
    /* Step 5: Restore context — this does NOT return (ends with mret) */
    j       _tx_thread_context_restore

    .size _tx_esp32c6_trap_handler, . - _tx_esp32c6_trap_handler
