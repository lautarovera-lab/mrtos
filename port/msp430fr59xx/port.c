/*
 * mRTOS port: MSP430FR59xx - implementation.
 *
 * NOTE on ISR declaration: this file uses
 *     __attribute__((naked, interrupt(VECTOR)))
 * which recent msp430-elf-gcc accepts (interrupt places the vector entry,
 * naked suppresses the compiler prologue so we fully control the frame).
 * If your toolchain rejects the combination, move the two ISR bodies to a
 * .S file and place the vectors manually:
 *     .section __interrupt_vector_<n>, "ax", @progbits
 *     .word    port_tick_isr
 * with <n> taken from the device header's vector numbering.
 */
#include <msp430.h>
#include "mrtos.h"

/* Dedicated stack for the C portion of kernel ISRs. */
static port_stack_t isr_stack[PORT_CFG_ISR_STACK_WORDS];
__attribute__((used))
port_stack_t * const port_isr_sp = &isr_stack[PORT_CFG_ISR_STACK_WORDS];

/* ------------------------------------------------------------------ */
/* Tick period (ACLK counts per kernel tick) and tickless limits.       */
/* ------------------------------------------------------------------ */
#define PORT_TICK_CYCLES  (PORT_TICK_CLK_HZ / MRTOS_CFG_TICK_HZ)
#if (PORT_TICK_CYCLES < 2) || (PORT_TICK_CYCLES > 65534)
#error "Tick period out of range for 16-bit Timer_A; adjust TICK_HZ/ACLK"
#endif
#if (PORT_TICK_CLK_HZ % MRTOS_CFG_TICK_HZ) != 0
/* A truncated divider would make every tick slightly short - the kernel's
 * time base would run fast with no visible error anywhere. Pick a TICK_HZ
 * that divides ACLK exactly (e.g. 32768/1024 = 32). */
#error "ACLK is not an integer multiple of TICK_HZ; tick would drift"
#endif

/* Largest whole-tick span programmable into CCR0 while staying strictly below
 * the parked CCR1 yield channel (0xFFFF). With 32 counts/tick that is
 * 0xFFFF/32 = 2047 ticks (CCR0 = 32*2047-1 = 0xFFDF). A deadline (or the
 * no-deadline case) farther out than this just re-arms on the capped wake. */
#define PORT_TICKLESS_MAX_TICKS  (0xFFFFu / PORT_TICK_CYCLES)

/* Word offset of the packed SR within a saved context frame: R4..R15 occupy
 * 12 regs * 2 words = 24 words, then {SR | PC[19:16]}. Used to clear idle's
 * LPM bits on wake so its loop re-runs. Must track PORT_ISR_BODY's save order. */
#define PORT_FRAME_SR_WORD  24u

/* Tickless idle state. Written only with interrupts disabled (idle arm) or in
 * the TA0 ISRs (reconcile), so no further guarding is needed. */
static uint8_t      tickless_armed;   /* a suppressed sleep is in progress     */
static uint16_t     tickless_span;    /* whole ticks the armed CCR0 represents  */
static uint16_t     tickless_carry;   /* dropped sub-tick ACLK counts (anti-drift) */
static mrtos_tcb_t *idle_self;        /* idle TCB, captured on its first run    */

/* ------------------------------------------------------------------ */
/* Critical sections                                                    */
/* ------------------------------------------------------------------ */
uint16_t port_irq_save(void)
{
    uint16_t key = __get_SR_register();
    __disable_interrupt();
    __no_operation();                          /* DINT takes effect next insn */
    return key;
}

void port_irq_restore(uint16_t key)
{
    if (key & GIE)
        __enable_interrupt();
}

/* ------------------------------------------------------------------ */
/* Yield request: pend the TA0 CCR1 interrupt (software-only source).   */
/* Safe from task context (fires when GIE is restored) and from ISRs    */
/* (fires after the outer RETI). MSP430's PendSV equivalent.            */
/* ------------------------------------------------------------------ */
void port_yield(void)
{
    TA0CCTL1 |= CCIFG;
    __no_operation();
}

/* ------------------------------------------------------------------ */
/* Initial task stack frame. Must match exactly what the restore path   */
/* (POPM.A #12,R15 + RETI) expects:                                     */
/*                                                                      */
/*   high addr ->  PC[15:0]                 (hardware frame)            */
/*                 SR | PC[19:16]<<12                                   */
/*                 R15 [19:16] / R15 [15:0] (PUSHM.A order: R15 first)  */
/*                 ...                                                  */
/*   sp ------->   R4  [19:16] / R4  [15:0]                             */
/* ------------------------------------------------------------------ */
port_stack_t *port_stack_init(port_stack_t *base, size_t words,
                              void (*shell)(void *), void *arg)
{
    uint16_t *sp = (uint16_t *)((uintptr_t)(base + words) & ~(uintptr_t)1);
    uint32_t  pc = (uint32_t)(uintptr_t)shell;
    int       r;

    *--sp = (uint16_t)pc;                                /* PC[15:0]        */
    *--sp = (uint16_t)(((pc >> 4) & 0xF000u) | GIE);     /* SR + PC[19:16]  */
    for (r = 15; r >= 4; --r) {
        *--sp = 0;                                       /* Rn bits 19:16   */
        *--sp = (r == 12) ? (uint16_t)(uintptr_t)arg     /* R12 = 1st arg   */
                          : 0;                           /* (msp430 ABI)    */
    }
    return sp;
}

/* ------------------------------------------------------------------ */
/* Tickless reconciliation helpers (run in TA0 ISR context, IRQs off).  */
/* ------------------------------------------------------------------ */

/* Fold an aborted suppressed sleep back into the kernel time base. `counts`
 * is the live TA0R at the early wake. Whole ticks advance the clock; the
 * sub-tick remainder accumulates in tickless_carry so the time base does not
 * creep slow across many short idle entries (each early wake would otherwise
 * discard up to 31 counts). Restores the periodic CCR0 and disarms. */
__attribute__((used)) static void port_tickless_unwind(uint16_t counts)
{
    uint16_t ticks = (uint16_t)(counts / PORT_TICK_CYCLES);
    tickless_carry = (uint16_t)(tickless_carry + counts % PORT_TICK_CYCLES);
    if (tickless_carry >= PORT_TICK_CYCLES) {
        ticks++;
        tickless_carry = (uint16_t)(tickless_carry - PORT_TICK_CYCLES);
    }
    TA0CTL  |= TACLR;                              /* phase -> 0            */
    TA0CCR0  = (uint16_t)(PORT_TICK_CYCLES - 1u);  /* back to periodic tick */
    tickless_armed = 0;
    if (ticks)
        mrtos_tick_advance(ticks);                 /* also re-picks         */
}

/* If the scheduler just (re)selected idle after a wake, clear the LPM bits in
 * its saved SR so it RETIs back ACTIVE and re-runs port_idle - re-reading the
 * deadline and pm cap - instead of dropping straight back to sleep on the
 * stale saved SR. Only idle ever sleeps via a port arm, so no other frame is
 * touched. Call after sched_pick, before the ISR restore. */
__attribute__((used)) static void port_idle_resume_fixup(void)
{
    if (idle_self != NULL && mrtos_cur == idle_self)
        ((uint16_t *)idle_self->sp)[PORT_FRAME_SR_WORD] &= (uint16_t)~LPM4_bits;
}

/* ------------------------------------------------------------------ */
/* Context switch ISRs.                                                 */
/* Frame on entry (hardware): PC + SR already pushed on the TASK stack. */
/* We push R4..R15, save SP into mrtos_cur->sp (offset 0), hop onto the */
/* ISR stack for the C handler (which may retarget mrtos_cur), then     */
/* restore from the - possibly different - mrtos_cur and RETI.          */
/* ------------------------------------------------------------------ */
#define PORT_ISR_BODY(c_handler)                                          \
    __asm__ __volatile__ (                                                \
        "pushm.a #12, r15            \n\t"  /* save R15..R4 (20-bit)   */ \
        "mov.w   &mrtos_cur, r12     \n\t"                                \
        "mov.w   sp, 0(r12)          \n\t"  /* TCB->sp = SP            */ \
        "mov.w   &port_isr_sp, sp    \n\t"  /* switch to ISR stack     */ \
        "call    #" c_handler "      \n\t"                                \
        "mov.w   &mrtos_cur, r12     \n\t"                                \
        "mov.w   @r12, sp            \n\t"  /* SP = (new) TCB->sp      */ \
        "popm.a  #12, r15            \n\t"                                \
        "reti                        \n\t")

/* CCR0 fired. If a suppressed sleep was armed, the programmed limit was just
 * reached: exactly tickless_span ticks elapsed and the counter wrapped to a
 * tick boundary (phase 0), so fold the full span and restore the periodic
 * period - no TACLR needed. Otherwise this is an ordinary active-mode tick. */
__attribute__((used)) static void port_tick_handler(void)
{
    if (tickless_armed) {
        uint16_t span = tickless_span;
        TA0CCR0 = (uint16_t)(PORT_TICK_CYCLES - 1u);
        tickless_armed = 0;
        mrtos_tick_advance(span);              /* advances clock + re-picks  */
    } else {
        mrtos_tick();
    }
    port_idle_resume_fixup();
}

__attribute__((naked, interrupt(TIMER0_A0_VECTOR)))
void port_tick_isr(void)
{
    PORT_ISR_BODY("port_tick_handler");
}

__attribute__((used)) static void port_yield_handler(void)
{
    TA0CCTL1 &= ~CCIFG;
    if (tickless_armed)                        /* woken early, mid suppressed sleep */
        port_tickless_unwind(TA0R);            /* fold partial sleep, restore tick  */
    mrtos_sched_pick();
    port_idle_resume_fixup();
}

__attribute__((naked, interrupt(TIMER0_A1_VECTOR)))
void port_yield_isr(void)
{
    PORT_ISR_BODY("port_yield_handler");
}

/* ------------------------------------------------------------------ */
/* Tick timer + first task launch.                                      */
/* ------------------------------------------------------------------ */
static void port_tick_timer_init(void)
{
    TA0CCR0  = (uint16_t)(PORT_TICK_CYCLES - 1u);
    TA0CCTL0 = CCIE;                           /* tick                     */
    TA0CCR1  = 0xFFFFu;                        /* unreachable in up mode:  */
    TA0CCTL1 = CCIE;                           /* CCIFG = software-only    */
    TA0CTL   = TASSEL__ACLK | MC__UP | TACLR;  /* ACLK, up mode, no divider */
}

__attribute__((naked, noreturn)) static void port_launch_first(void)
{
    __asm__ __volatile__ (
        "mov.w  &mrtos_cur, r12      \n\t"
        "mov.w  @r12, sp             \n\t"
        "popm.a #12, r15             \n\t"
        "reti                        \n\t");
    __builtin_unreachable();
}

void port_start(void)
{
    __disable_interrupt();
    port_tick_timer_init();
    port_launch_first();                       /* RETI sets GIE from frame */
}

/* ------------------------------------------------------------------ */
/* Idle: tickless LPM sleep straight to the next deadline.              */
/*                                                                      */
/* The tick runs from ACLK (LFXT), which survives LPM3, so the timer    */
/* keeps the kernel time base while the CPU and SMCLK/DCO are off. The  */
/* win over a periodic LPM3 idle is that we do NOT wake every tick: we  */
/* program CCR0 for the whole span to the next pending deadline (or the */
/* re-arm cap when nothing is pending) so the DCO restart + context     */
/* save are paid once per deadline instead of 1024x/s.                  */
/*                                                                      */
/* Wake handling (the silicon-risky part):                              */
/*  - Planned wake (CCR0 reaches its limit): port_tick_handler folds    */
/*    the full tickless_span; the counter wrapped to a tick boundary.   */
/*  - Early wake (a peripheral readies a task and pends the CCR1 yield): */
/*    port_yield_handler unwinds from the live TA0R, folding the whole  */
/*    ticks and carrying the sub-tick remainder. Reconciliation happens */
/*    in ISR context, BEFORE the woken task runs, so mrtos_now() and the */
/*    delay list are already consistent when it observes them.          */
/*  - Either way, if the scheduler re-selects idle, its saved LPM bits  */
/*    are cleared (port_idle_resume_fixup) so this function re-runs and  */
/*    re-evaluates the deadline / pm cap rather than re-sleeping blind.  */
/*                                                                      */
/* The sleep depth honors mrtos_pm_max_lpm(): a driver that needs       */
/* clocks (e.g. LEA at LPM0) caps how deep we go; the cap is re-read on  */
/* every entry. LPM4 is never used - it stops ACLK and would freeze the */
/* tick. The arm + LPM entry is made race-free by a single BIS that     */
/* sets the LPM bits together with GIE (any IRQ pending in between is    */
/* taken only after the CPU is already asleep - no lost wakeup).        */
/* ------------------------------------------------------------------ */
void port_idle(void)
{
    if (idle_self == NULL)
        idle_self = mrtos_cur;             /* capture once: we ARE idle here */

    __disable_interrupt();
    __no_operation();                      /* DINT effective on next insn    */

    uint16_t bits;
    switch (mrtos_pm_max_lpm()) {          /* deepest mode no one has vetoed */
    case MRTOS_LPM0: bits = LPM0_bits; break;
    case MRTOS_LPM1: bits = LPM1_bits; break;
    case MRTOS_LPM2: bits = LPM2_bits; break;
    default:         bits = LPM3_bits; break;
    }

    uint16_t d = mrtos_next_deadline();    /* ticks to earliest wake, 0=none */
    if (d == 0u || d > PORT_TICKLESS_MAX_TICKS)
        d = PORT_TICKLESS_MAX_TICKS;       /* nothing pending / too far: re-arm */
    tickless_span  = d;
    /* No TACLR: the current sub-tick phase is preserved, so the first tick of
     * the span consumes only its remaining counts and the planned wake lands
     * exactly tickless_span ticks on. */
    TA0CCR0        = (uint16_t)(d * PORT_TICK_CYCLES - 1u);
    tickless_armed = 1;

    __bis_SR_register(bits | GIE);         /* atomic arm-to-sleep, GIE on    */
    __no_operation();
    /* Resumed active: the wake was reconciled in the TA0 ISR and the LPM bits
     * were cleared from this frame. idle_entry's loop calls us again to set up
     * the next suppressed sleep. */
}
