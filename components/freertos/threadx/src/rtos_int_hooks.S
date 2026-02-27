/*
 * rtos_int_hooks.S — ESP-IDF interrupt dispatch hooks for ThreadX
 *
 * ESP-IDF's vectors.S calls rtos_int_enter/rtos_int_exit for all
 * interrupts that go through _interrupt_handler. These manage:
 *   - ISR nesting counter (port_uxInterruptNesting)
 *   - ThreadX system state (_tx_thread_system_state) — CRITICAL for Bug 36
 *   - Stack switching (task stack → ISR stack on first-level interrupt)
 *   - Context switch check on ISR exit (xPortSwitchFlag + vTaskSwitchContext)
 *
 * Calling convention (from vectors.S):
 *   rtos_int_enter:
 *     - No arguments (SP already has saved context frame)
 *     - Returns "context" in a0 (0 on ESP32-C6, no coprocessors)
 *     - May switch SP from task stack to ISR stack
 *
 *   rtos_int_exit:
 *     - a0 = saved mstatus (to restore after ISR)
 *     - a1 = context from rtos_int_enter (unused on ESP32-C6)
 *     - Returns new mstatus in a0
 *     - At last ISR level: checks xPortSwitchFlag, calls vTaskSwitchContext,
 *       then ALWAYS restores SP from pxCurrentTCBs (matching FreeRTOS pattern)
 *
 * Global variables:
 *   port_xSchedulerRunning    — non-zero when ThreadX scheduler is active
 *   port_uxInterruptNesting   — ISR nesting depth (FreeRTOS compat)
 *   _tx_thread_system_state   — ThreadX ISR nesting (Bug 36: MUST be >0 in ISR)
 *   xPortSwitchFlag           — set by portYIELD_FROM_ISR to request switch
 *   port_uxCriticalNesting    — critical section nesting (for FreeRTOS compat)
 *   pxCurrentTCBs             — pointer to SP save area (offset 0 = saved SP)
 *   xIsrStackTop              — top of ISR stack
 *
 * These match the names used by ESP-IDF's real FreeRTOS port, so that
 * ESP-IDF components referencing them (espcoredump, gdbstub) link correctly.
 *
 * Bug 36: _tx_thread_system_state MUST be incremented in rtos_int_enter and
 * decremented in rtos_int_exit. Without this, ThreadX APIs called from ESP-IDF
 * ISR handlers (e.g., tx_semaphore_put from WiFi ISR via xSemaphoreGive compat)
 * see _tx_thread_system_state==0 and think they're in thread context. They then
 * attempt immediate preemption via _tx_thread_system_return(), which corrupts
 * the scheduler because it runs on the ISR stack, not the thread stack.
 *
 * ISR exit follows the FreeRTOS pattern (not ThreadX _tx_thread_system_preempt_check):
 *   1. Decrement nesting counters
 *   2. At last ISR level: check xPortSwitchFlag → vTaskSwitchContext
 *   3. Restore SP from pxCurrentTCBs (undoes the stack switch from rtos_int_enter)
 *
 * Bug 37 lesson: DO NOT call _tx_thread_system_preempt_check() from rtos_int_exit.
 * That function calls _tx_thread_system_return() which creates a "solicited" context
 * frame on the ISR stack — fundamentally incompatible with ESP-IDF's interrupt frame
 * format. The two frame types are different sizes and layouts, causing corruption.
 */

    .section .text
    .align 4

    /* ThreadX system state — defined in ThreadX common/src/tx_thread_initialize.c */
    .extern _tx_thread_system_state
    .extern vTaskSwitchContext

/* ── rtos_int_enter ──────────────────────────────────────────────── */
    .global rtos_int_enter
    .type   rtos_int_enter, @function
rtos_int_enter:
    /* Check if scheduler is running */
    lw      t0, port_xSchedulerRunning
    beqz    t0, .Lenter_end             /* Not running — return 0, keep stack */

    /* Increment ISR nesting counter (FreeRTOS compat) */
    la      t3, port_uxInterruptNesting
    lw      t4, 0(t3)                   /* t4 = current nesting */
    addi    t5, t4, 1
    sw      t5, 0(t3)                   /* store incremented value */

    /* Bug 36: Increment ThreadX system state so TX APIs know we're in ISR.
     * Without this, tx_semaphore_put etc. see system_state==0, think they're
     * in thread context, and call _tx_thread_system_return() — corrupting
     * the scheduler because we're actually on the ISR stack. */
    la      t3, _tx_thread_system_state
    lw      t5, 0(t3)
    addi    t5, t5, 1
    sw      t5, 0(t3)

    /* If already nested (t4 > 0), skip stack switch */
    bnez    t4, .Lenter_end

    /* First-level interrupt: save current SP in save area, switch to ISR stack.
     * pxCurrentTCBs points to a save area whose first word is the saved SP.
     * This matches the FreeRTOS TCB layout (offset 0 = pxTopOfStack). */
    lw      t0, pxCurrentTCBs
    beqz    t0, .Lenter_end             /* Defensive: no save area */
    sw      sp, 0(t0)                   /* save_area[0] = sp */
    lw      sp, xIsrStackTop            /* Switch to ISR stack */

.Lenter_end:
    li      a0, 0                       /* Return 0 (no coprocessors) */
    ret
    .size rtos_int_enter, . - rtos_int_enter


/* ── rtos_int_exit ───────────────────────────────────────────────── */
    .global rtos_int_exit
    .type   rtos_int_exit, @function

rtos_int_exit:
    /* Save mstatus (a0) in callee-saved register s11.
     * s11 is saved/restored by vectors.S save_general_regs/restore_general_regs,
     * so it's preserved across the entire ISR handler. */
    mv      s11, a0

    /* Check if scheduler is running */
    lw      t0, port_xSchedulerRunning
    beqz    t0, .Lexit_end              /* Not running — return mstatus */

    /* Decrement ISR nesting counter */
    la      t2, port_uxInterruptNesting
    lw      t3, 0(t2)
    beqz    t3, .Lskip_dec              /* Already 0 (shouldn't happen) */
    addi    t3, t3, -1
    sw      t3, 0(t2)
.Lskip_dec:

    /* Bug 36: Decrement ThreadX system state (mirrors the increment in
     * rtos_int_enter). This restores the system_state to its pre-ISR value
     * so ThreadX APIs return to normal thread-context behavior. */
    la      t2, _tx_thread_system_state
    lw      t4, 0(t2)
    beqz    t4, .Lskip_sys_dec          /* Already 0 (shouldn't happen) */
    addi    t4, t4, -1
    sw      t4, 0(t2)
.Lskip_sys_dec:

    /* If still nested (nesting > 0), keep ISR stack — skip switch check */
    bnez    t3, .Lexit_end

    /* Exiting last ISR level.
     * Check xPortSwitchFlag — set by portYIELD_FROM_ISR when an ISR handler
     * wants to trigger a context switch. Call vTaskSwitchContext if set.
     * Currently portYIELD_FROM_ISR is a no-op and vTaskSwitchContext is a stub,
     * so this path is effectively dead code. It's kept for FreeRTOS pattern
     * compatibility and future ISR-level preemption support. */
    la      t0, xPortSwitchFlag
    lw      t2, 0(t0)
    beqz    t2, .Lno_switch

    /* Context switch requested — call vTaskSwitchContext, then clear flag.
     * Save/restore ra on stack (we're still on ISR stack here). */
    addi    sp, sp, -16
    sw      ra, 0(sp)
    li      a0, 0                       /* xCoreID = 0 */
    call    vTaskSwitchContext
    lw      ra, 0(sp)
    addi    sp, sp, 16
    la      t0, xPortSwitchFlag
    sw      zero, 0(t0)                 /* Clear the flag */

.Lno_switch:
    /* ALWAYS restore SP from pxCurrentTCBs at last ISR level exit.
     * This undoes the stack switch done in rtos_int_enter.
     * Matches the real FreeRTOS rtos_int_exit pattern exactly. */
    lw      t0, pxCurrentTCBs
    beqz    t0, .Lexit_end              /* Defensive: no save area */
    lw      sp, 0(t0)                   /* Restore task SP from save area */

.Lexit_end:
    mv      a0, s11                     /* Return mstatus */
    ret
    .size rtos_int_exit, . - rtos_int_exit


/* ── Global variables ────────────────────────────────────────────── */
    .section .data

    .global port_xSchedulerRunning
    .type   port_xSchedulerRunning, @object
    .align  4
port_xSchedulerRunning:
    .word   0
    .size   port_xSchedulerRunning, 4

    .global port_uxInterruptNesting
    .type   port_uxInterruptNesting, @object
    .align  4
port_uxInterruptNesting:
    .word   0
    .size   port_uxInterruptNesting, 4

    .global xPortSwitchFlag
    .type   xPortSwitchFlag, @object
    .align  4
xPortSwitchFlag:
    .word   0
    .size   xPortSwitchFlag, 4

    .global port_uxCriticalNesting
    .type   port_uxCriticalNesting, @object
    .align  4
port_uxCriticalNesting:
    .word   0
    .size   port_uxCriticalNesting, 4

    /* pxCurrentTCBs — pointer to SP save area.
     * Points to _current_task_sp_save, a single word that holds the saved SP.
     * rtos_int_enter writes the task SP here before switching to ISR stack.
     * rtos_int_exit reads it back to restore the task SP.
     *
     * This matches the FreeRTOS TCB layout where offset 0 = pxTopOfStack.
     * On single-core ESP32-C6, only one thread can be interrupted at a time,
     * so a single save word (not per-thread) is sufficient.
     *
     * ESP-IDF components (espcoredump, gdbstub) reference pxCurrentTCBs
     * to find the current task's stack — the save area satisfies this. */
    .global _current_task_sp_save
    .type   _current_task_sp_save, @object
    .align  4
_current_task_sp_save:
    .word   0                           /* Saved task SP (written by rtos_int_enter) */
    .size   _current_task_sp_save, 4

    .global pxCurrentTCBs
    .type   pxCurrentTCBs, @object
    .align  4
pxCurrentTCBs:
    .word   _current_task_sp_save       /* Points to save area (non-NULL when linked) */
    .size   pxCurrentTCBs, 4

    /* ISR stack — allocate a dedicated stack for interrupt handlers.
     * This is separate from ThreadX's system stack (_tx_thread_system_stack_ptr)
     * because ESP-IDF's vectors.S manages stack switching independently. */
    .section .bss
    .align  16
    .global xIsrStack
xIsrStack:
    .space  4096                        /* 4KB ISR stack */

    .section .data
    .global xIsrStackTop
    .type   xIsrStackTop, @object
    .align  4
xIsrStackTop:
    .word   xIsrStack + 4096            /* Stack grows down — top = end */
    .size   xIsrStackTop, 4

    .global xIsrStackBottom
    .type   xIsrStackBottom, @object
    .align  4
xIsrStackBottom:
    .word   xIsrStack                   /* Bottom = start */
    .size   xIsrStackBottom, 4
