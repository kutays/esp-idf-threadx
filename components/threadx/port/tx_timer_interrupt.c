/*
 * tx_timer_interrupt.c — ESP32-C6 SYSTIMER tick for ThreadX
 *
 * ESP32-C6 does NOT have standard RISC-V mtime/mtimecmp. Instead it uses
 * a SYSTIMER peripheral with 52-bit counter and 3 comparators. We use
 * comparator 0 (COMP0) for the ThreadX tick.
 *
 * The SYSTIMER runs at 16 MHz (XTAL_CLK on ESP32-C6).
 *
 * Interrupt routing:
 *   SYSTIMER_TARGET0 → Interrupt Matrix (INTMTX) → CPU interrupt line N
 *   CPU interrupt control: PLIC machine-mode at 0x20001000
 *
 * NOTE: The ESP32-C6 has SOC_INT_PLIC_SUPPORTED=y. The machine-mode
 * CPU interrupt controller is the PLIC at DR_REG_PLIC_MX_BASE (0x20001000),
 * NOT the INTPRI peripheral at 0x600C5000. mie.MEIE must also be set.
 */

#include "tx_api.h"
#include "tx_timer.h"

#include "soc/soc.h"
#include "soc/systimer_reg.h"
#include "soc/interrupt_matrix_reg.h"

/* _tx_timer_interrupt is defined in ports/risc-v64/gnu/src/tx_timer_interrupt.c
   but not declared in any public ThreadX header. */
extern VOID _tx_timer_interrupt(VOID);

/* SYSTIMER clock frequency on ESP32-C6: 16 MHz */
#define SYSTIMER_CLK_FREQ           16000000

/* CPU interrupt number for our timer tick */
#define TIMER_CPU_INT_NUM           7

/* Interrupt priority (1–15, must be > threshold to fire) */
#define TIMER_INT_PRIORITY          1

/* Alarm period in SYSTIMER ticks */
#define TIMER_ALARM_PERIOD          (SYSTIMER_CLK_FREQ / TX_TIMER_TICKS_PER_SECOND)

/*
 * SYSTIMER_TARGET0 interrupt source number.
 * From soc/esp32c6/include/soc/interrupts.h:
 *   ETS_SYSTIMER_TARGET0_INTR_SOURCE = 57
 */
#define SYSTIMER_TARGET0_INT_SOURCE  57

/*
 * Interrupt Matrix (INTMTX) — maps peripheral sources to CPU interrupt lines.
 * Base: DR_REG_INTERRUPT_MATRIX_BASE = 0x60010000
 * Map register for source N: base + N * 4
 */
#define INTMTX_BASE                 DR_REG_INTERRUPT_MATRIX_BASE
#define INTMTX_MAP_REG(src)         (INTMTX_BASE + (src) * 4)

/*
 * PLIC Machine-mode CPU interrupt controller — DR_REG_PLIC_MX_BASE = 0x20001000
 *
 * This is the correct register set for controlling CPU interrupt lines
 * when running in machine mode on ESP32-C6 (SOC_INT_PLIC_SUPPORTED=y).
 * The INTPRI peripheral at 0x600C5000 is separate and NOT what we use.
 */
#define PLIC_MX_BASE                0x20001000
#define PLIC_MX_ENABLE_REG          (PLIC_MX_BASE + 0x00)  /* Enable bits per line */
#define PLIC_MX_TYPE_REG            (PLIC_MX_BASE + 0x04)  /* 0=level, 1=edge */
#define PLIC_MX_CLEAR_REG           (PLIC_MX_BASE + 0x08)  /* Clear edge-triggered */
#define PLIC_MX_EIP_STATUS_REG      (PLIC_MX_BASE + 0x0C)  /* Pending status (RO) */
#define PLIC_MX_PRI_REG(n)          (PLIC_MX_BASE + 0x10 + (n) * 4)  /* Per-line priority */
#define PLIC_MX_THRESH_REG          (PLIC_MX_BASE + 0x90)  /* Priority threshold */

/*
 * Global: which CPU interrupt line we're using for the timer.
 * Referenced from the assembly trap handler.
 */
UINT _tx_esp32c6_timer_cpu_int = TIMER_CPU_INT_NUM;

static inline void write_reg(uint32_t addr, uint32_t val)
{
    *(volatile uint32_t *)addr = val;
}

static inline uint32_t read_reg(uint32_t addr)
{
    return *(volatile uint32_t *)addr;
}

/*
 * _tx_port_setup_timer_interrupt
 *
 * Called from tx_initialize_low_level.S during ThreadX init.
 * Configures SYSTIMER COMP0 for periodic alarm and routes the interrupt
 * through the interrupt matrix to a CPU interrupt line via PLIC.
 *
 * The caller (assembly) must also set mie.MEIE (bit 11) to enable
 * machine external interrupts at the CSR level.
 */
void _tx_port_setup_timer_interrupt(void)
{
    /* --- Step 1: Configure SYSTIMER COMP0 --- */

    /*
     * Enable SYSTIMER clock (bit 0 = CLK_FO force-on) and
     * COMP0 work enable (bit 24 = TARGET0_WORK_EN).
     * Both must be set for comparator 0 to fire.
     */
    write_reg(SYSTIMER_CONF_REG,
              read_reg(SYSTIMER_CONF_REG) | (1 << 0) | (1 << 24));

    /* Disable COMP0 while configuring */
    write_reg(SYSTIMER_TARGET0_CONF_REG, 0);

    /*
     * SYSTIMER_TARGET0_CONF_REG bitfields:
     *   [25:0]  TARGET0_PERIOD      — alarm period in SYSTIMER ticks
     *   [30]    TARGET0_PERIOD_MODE — 1 = periodic (auto-reload)
     *   [31]    TARGET0_TIMER_UNIT_SEL — 0 = use counter unit 0 (leave 0!)
     *
     * Do NOT set bit 31: that would select timer unit 1 instead of unit 0.
     */
    write_reg(SYSTIMER_TARGET0_CONF_REG,
              (TIMER_ALARM_PERIOD & SYSTIMER_TARGET0_PERIOD_V) |
              SYSTIMER_TARGET0_PERIOD_MODE);

    /* Trigger load: copies period from conf reg into the hardware comparator */
    write_reg(SYSTIMER_COMP0_LOAD_REG, 1);

    /* Enable COMP0 interrupt in SYSTIMER */
    write_reg(SYSTIMER_INT_ENA_REG, read_reg(SYSTIMER_INT_ENA_REG) | (1 << 0));

    /* Clear any stale pending interrupt */
    write_reg(SYSTIMER_INT_CLR_REG, (1 << 0));

    /* --- Step 2: Route through Interrupt Matrix --- */

    /*
     * Write CPU interrupt number to the SYSTIMER_TARGET0 map register.
     * Map register for source 57: INTMTX_BASE + 57*4 = 0x60010000 + 0xe4
     */
    write_reg(INTMTX_MAP_REG(SYSTIMER_TARGET0_INT_SOURCE), TIMER_CPU_INT_NUM);

    /* --- Step 3: Configure CPU interrupt via PLIC machine-mode registers --- */

    /* Set edge-triggered type for our CPU interrupt line */
    write_reg(PLIC_MX_TYPE_REG,
              read_reg(PLIC_MX_TYPE_REG) | (1u << TIMER_CPU_INT_NUM));

    /* Set priority (must be > threshold to fire) */
    write_reg(PLIC_MX_PRI_REG(TIMER_CPU_INT_NUM), TIMER_INT_PRIORITY);

    /* Enable our CPU interrupt line */
    write_reg(PLIC_MX_ENABLE_REG,
              read_reg(PLIC_MX_ENABLE_REG) | (1u << TIMER_CPU_INT_NUM));

    /* Threshold = 0 so all priority >= 1 interrupts are unmasked */
    write_reg(PLIC_MX_THRESH_REG, 0);
}

/*
 * _tx_esp32c6_timer_isr
 *
 * Called from the assembly trap handler when SYSTIMER COMP0 fires.
 */
void _tx_esp32c6_timer_isr(void)
{
    /* Clear SYSTIMER COMP0 interrupt */
    write_reg(SYSTIMER_INT_CLR_REG, (1 << 0));

    /* Clear the edge-triggered CPU interrupt latch in PLIC */
    write_reg(PLIC_MX_CLEAR_REG, (1u << TIMER_CPU_INT_NUM));

    /* Advance the ThreadX tick */
    _tx_timer_interrupt();
}
