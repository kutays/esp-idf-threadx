/*
 * rtos_int_hooks.S — ESP-IDF interrupt dispatch hooks for ThreadX
 *
 * ESP-IDF's vectors.S calls rtos_int_enter/rtos_int_exit for all
 * interrupts that go through _interrupt_handler. These manage:
 *   - ISR nesting counter (port_uxInterruptNesting)
 *   - ThreadX system state (_tx_thread_system_state) — CRITICAL for Bug 36
 *   - Stack switching (task stack → ISR stack on first-level interrupt)
 *   - Context switch check on ISR exit
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
 *     - Restores SP from task TCB (may be different task if switch occurred)
 *
 * Global variables:
 *   port_xSchedulerRunning    — non-zero when ThreadX scheduler is active
 *   port_uxInterruptNesting   — ISR nesting depth (FreeRTOS compat)
 *   _tx_thread_system_state   — ThreadX ISR nesting (Bug 36: MUST be >0 in ISR)
 *   xPortSwitchFlag           — set by portYIELD_FROM_ISR to request switch
 *   port_uxCriticalNesting    — critical section nesting (for FreeRTOS compat)
 *   pxCurrentTCBs             — pointer to current task TCB (offset 0 = saved SP)
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
 * ThreadX's own timer ISR path (vector[17]) manages _tx_thread_system_state via
 * _tx_thread_context_save (increment) and _tx_thread_context_restore (decrement).
 * The ESP-IDF path (vector[N≠17]) goes through _interrupt_handler → rtos_int_enter
 * → handler → rtos_int_exit, which is completely separate. Both paths must keep
 * _tx_thread_system_state in sync:
 *   - ThreadX timer nests into ESP-IDF ISR: state goes 1→2 (context_save) then 2→1
 *     (context_restore), then 1→0 (rtos_int_exit). Correct.
 *   - ESP-IDF ISR only: state goes 0→1 (rtos_int_enter), APIs see ISR context,
 *     then 1→0 (rtos_int_exit). Correct.
 */

    .section .text
    .align 4

    /* ThreadX system state — defined in ThreadX common/src/tx_thread_initialize.c */
    .extern _tx_thread_system_state

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

    /* First-level interrupt: save current SP in TCB, switch to ISR stack.
     * pxCurrentTCBs points to the current task's TCB. By FreeRTOS convention,
     * offset 0 of TCB = pxTopOfStack (the saved stack pointer). */
    lw      t0, pxCurrentTCBs
    beqz    t0, .Lenter_end             /* Defensive: no current task */
    sw      sp, 0(t0)                   /* TCB->pxTopOfStack = sp */
    lw      sp, xIsrStackTop            /* Switch to ISR stack */

.Lenter_end:
    li      a0, 0                       /* Return 0 (no coprocessors) */
    ret
    .size rtos_int_enter, . - rtos_int_enter


/* ── rtos_int_exit ───────────────────────────────────────────────── */
    .global rtos_int_exit
    .type   rtos_int_exit, @function
rtos_int_exit:
    /* Save mstatus (a0) in callee-saved register */
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

    /* If still nested, keep ISR stack */
    bnez    t3, .Lexit_end

    /* Exiting last ISR level — check for pending context switch */
    la      t0, xPortSwitchFlag
    lw      t2, 0(t0)
    beqz    t2, .Lno_switch

    /* Context switch requested — call vTaskSwitchContext.
     * This updates pxCurrentTCBs to point to the next task to run. */
    addi    sp, sp, -16
    sw      ra, 0(sp)
    li      a0, 0                       /* core ID = 0 */
    call    vTaskSwitchContext
    lw      ra, 0(sp)
    addi    sp, sp, 16

    /* Clear switch flag */
    la      t0, xPortSwitchFlag
    sw      zero, 0(t0)

.Lno_switch:
    /* Restore task SP from (possibly new) current TCB */
    lw      t0, pxCurrentTCBs
    beqz    t0, .Lexit_end              /* Defensive: no current task */
    lw      sp, 0(t0)                   /* sp = TCB->pxTopOfStack */

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

    /* pxCurrentTCBs — pointer to current task TCB.
     * For ThreadX, we maintain this to point at a structure whose
     * first member (offset 0) is the saved stack pointer. This matches
     * the FreeRTOS TCB layout that vectors.S expects. */
    .global pxCurrentTCBs
    .type   pxCurrentTCBs, @object
    .align  4
pxCurrentTCBs:
    .word   0
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
