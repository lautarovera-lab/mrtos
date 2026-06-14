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

__attribute__((naked, interrupt(TIMER0_A0_VECTOR)))
void port_tick_isr(void)
{
    PORT_ISR_BODY("mrtos_tick");
}

__attribute__((used)) static void port_yield_handler(void)
{
    TA0CCTL1 &= ~CCIFG;
    mrtos_sched_pick();
}

__attribute__((naked, interrupt(TIMER0_A1_VECTOR)))
void port_yield_isr(void)
{
    PORT_ISR_BODY("port_yield_handler");
}

/* ------------------------------------------------------------------ */
/* Tick timer + first task launch.                                      */
/* ------------------------------------------------------------------ */
#define PORT_TICK_CYCLES  (PORT_TICK_CLK_HZ / MRTOS_CFG_TICK_HZ)
#if (PORT_TICK_CYCLES < 2) || (PORT_TICK_CYCLES > 65534)
#error "Tick period out of range for 16-bit Timer_A; adjust TICK_HZ/ACLK"
#endif
#if (PORT_TICK_CLK_HZ % MRTOS_CFG_TICK_HZ) != 0
/* A truncated divider would make every tick slightly short - the
 * kernel's time base would run fast with no visible error anywhere.
 * Pick a TICK_HZ that divides ACLK exactly (e.g. 32768/1024 = 32). */
#error "ACLK is not an integer multiple of TICK_HZ; tick would drift"
#endif

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
/* Idle: LPM3. The tick runs from ACLK (LFXT), which survives LPM3, so  */
/* the periodic tick keeps the kernel time base exact while the CPU and */
/* SMCLK/DCO are off between ticks - this is what drops the idle floor  */
/* from LPM0's ~277 uA to the LPM3 single-digit-uA range. Any interrupt */
/* (the ACLK tick, or a peripheral such as the button) wakes the CPU -  */
/* MCLK/DCO restart automatically to run the ISR - and the scheduler    */
/* decides on RETI; idle's saved SR carries the LPM3 bits, so being     */
/* re-scheduled transparently re-enters LPM3. LPM4 is intentionally NOT */
/* used: it stops ACLK and would freeze the tick.                       */
/*                                                                      */
/* NOTE: this still wakes every tick (1024 Hz). Honoring a dynamic      */
/* mrtos_pm_max_lpm() cap and suppressing the between-deadline ticks    */
/* (true tickless, mrtos_next_deadline/mrtos_tick_advance) both need    */
/* idle restructured to re-evaluate on each sleep - tracked as the next */
/* increment, gated on the measured LPM3-idle current.                  */
/* ------------------------------------------------------------------ */
void port_idle(void)
{
    __bis_SR_register(LPM3_bits);
}
